/**
 * @file nw_osi.c
 * The adapter between Espressif's radio libraries and nornOS: the wifi_osi_funcs_t table
 * (ESP-IDF's esp_adapter.c for the ESP32, written for nornOS primitives), the two tasks
 * that stand in for the FreeRTOS tasks the libraries create (the radio task and the
 * service task that runs timer callbacks and delivers events), a heap wrapper with
 * size headers, queues with front insertion, recursive mutexes on binary semaphores, the
 * ETS timers over nornOS software timers, and the interrupt trampoline.
 *
 * Interrupts: the library routes a peripheral source to a CPU interrupt and installs a
 * handler with an argument; nornOS stores a plain handler per CPU interrupt, so one
 * trampoline looks up the handler and argument and counts the nesting for _is_from_isr.
 */

#include <string.h>
#include "rtos.h"
#include "rtos_assert.h"
#include "port.h"
#include "nw_internal.h"
#include "esp_err.h"
#include "esp_private/wifi_os_adapter.h"
#include "esp_private/wifi.h"
#include "esp_event_base.h"


/*
 * Defines
 */

#define TAG                     "osi"
#define TIMER_MAGIC             0x5A6B7C8Dul    /* in ETSTimer.timer_period once set up */
#define EV_TIMER                0x0001u         /* service task notifications */
#define EV_EVENT                0x0002u
#define PD_PASS                 1
#define PD_FAIL                 0
#define MS_TO_TICKS(ms)         ((rtos_tick_t)(((uint32_t)(ms) * RTOS_CFG_TICK_HZ) / 1000ul))

#if RTOS_CFG_EVT_BITS < 32
# error nornWIF needs RTOS_CFG_EVT_BITS 32: the radio library uses event group bits above 16
#endif
#if !RTOS_CFG_HEAP
# error nornWIF needs RTOS_CFG_HEAP 1
#endif
#if RTOS_CFG_TIMER_TASK_PRIO != 0
# error nornWIF expects timer callbacks in the tick (RTOS_CFG_TIMER_TASK_PRIO 0)
#endif
#if NW_CFG_TASK_WIFI > RTOS_CFG_NUM_TASKS  ||  NW_CFG_TASK_SERVICE < 1  ||  NW_CFG_TASK_SERVICE >= NW_CFG_TASK_WIFI
# error NW_CFG_TASK_WIFI and NW_CFG_TASK_SERVICE must be two tasks of the application, the radio task the higher one
#endif


/*
 * Local type definitions
 */

/** ETS timer as the library declares it (rom/ets_sys.h); timer_expire holds our
    record, timer_period the magic. */
typedef struct etsTimer
{
    struct etsTimer *timer_next;
    uint32_t timer_expire;
    uint32_t timer_period;
    void (*timer_func)(void *);
    void *timer_arg;
} etsTimer_t;

typedef struct nwQueue
{
    uint8_t *buf;
    uint32_t itemSize, cap, count, head, tail;
    rtos_sem_t items;
    uint32_t dropped;
} nwQueue_t;

typedef struct nwMutex
{
    rtos_sem_t sem;
    uint8_t owner;
    uint8_t depth;
    bool recursive;
} nwMutex_t;

typedef struct heapHdr
{
    uint32_t size;
    uint32_t magic;
} heapHdr_t;

typedef struct event
{
    int32_t id;
    uint16_t size;
    uint8_t data[NW_CFG_EVENT_DATA_MAX];
} event_t;


/*
 * Local data
 */

static uint8_t _stackWifi[NW_CFG_WIFI_STACK] __attribute__((aligned(16)));
static uint8_t _stackService[NW_CFG_SERVICE_STACK] __attribute__((aligned(16)));
static void (*_wifiFn)(void *) = NULL;
static void *_wifiArg;
static uint8_t _wifiHandle;             /* its address is the task handle */

static rtos_sem_t _threadSem[RTOS_NUM_TASKS];
static bool _threadSemReady[RTOS_NUM_TASKS];

static nwTimer_t *_pendingHead = NULL, *_pendingTail = NULL;

static event_t _events[NW_CFG_EVENT_QUEUE];
static uint8_t _evHead = 0, _evCount = 0;

static struct
{
    void (*fn)(void *);
    void *arg;
    uint8_t source;
} _isr[32];
static uint32_t _isrMask = 0;
static volatile uint8_t _isrDepth = 0;

static uint32_t _statEvents, _statEventsDropped, _statTimerCallbacks, _statIsrs;

/* Diagnostics of the interrupt to task path: the longest pause in radio interrupts (with
   the CPU's interrupt registers at that moment), the longest time from a post out of an
   interrupt into an empty queue until some task took from a queue (approximates the
   radio task's wake-up latency), items that piled up behind an unread one, and per
   queue the drops and the semaphore gives the kernel had to discard. */
static volatile int64_t _diagLastIsrUs, _diagPostUs;
static volatile uint32_t _diagGaps, _diagMaxGapMs, _diagGapAtMs, _diagIntenableAtGap, _diagInterruptAtGap;
static volatile uint32_t _diagMaxWakeUs, _diagBacklog;
static volatile uint8_t _diagPostPending, _diagInGap;
static rtos_timer_t _diagTimer;
static struct nwQueue *_diagQueues[8];
static uint8_t _diagNumQueues;
static uint16_t _diagIsrsPerSec[40];          /* radio interrupts in each second since start */
static rtos_timer_t _diagJitterTimer;         /* every tick: how late the tick interrupt ran */
static volatile int64_t _diagLastTickUs;
static volatile uint32_t _diagTickMaxLateUs, _diagTicksLate200;
static uint32_t _diagIsrsAtSec;
static uint8_t _diagTicks, _diagSec;

extern void nw_printf_init(void);


/*
 * Special registers
 */

static inline uint32_t rsrInterrupt(void) { uint32_t v; __asm__ __volatile__ ("rsr %0, interrupt" : "=a"(v)); return v; }
static inline uint32_t rsrIntenable(void) { uint32_t v; __asm__ __volatile__ ("rsr %0, intenable" : "=a"(v)); return v; }
static inline void wsrIntenable(uint32_t v) { __asm__ __volatile__ ("wsr %0, intenable\n\trsync" : : "a"(v) : "memory"); }


/*
 * Heap
 */

void *nw_malloc(size_t n)
{
    heapHdr_t *pH = rtos_malloc(n + sizeof(heapHdr_t));
    if(pH == NULL)
    {
        NW_LOGE(TAG, "malloc %u failed, %lu free", (unsigned)n, (unsigned long)nw_heapFree());
        return NULL;
    }
    pH->size = (uint32_t)n;
    pH->magic = 0x4E574D41ul;
    return pH + 1;
}

void *nw_calloc(size_t count, size_t size)
{
    void *p = nw_malloc(count * size);
    if(p != NULL)
        memset(p, 0, count * size);
    return p;
}

void nw_free(void *p)
{
    if(p != NULL)
    {
        heapHdr_t *pH = (heapHdr_t *)p - 1;
        RTOS_ASSERT(pH->magic == 0x4E574D41ul);
        pH->magic = 0;
        rtos_free(pH);
    }
}

void *nw_realloc(void *p, size_t n)
{
    void *q;
    uint32_t old;
    if(p == NULL)
        return nw_malloc(n);
    if(n == 0)
    {
        nw_free(p);
        return NULL;
    }
    old = ((heapHdr_t *)p - 1)->size;
    q = nw_malloc(n);
    if(q != NULL)
    {
        memcpy(q, p, old < n ? old : n);
        nw_free(p);
    }
    return q;
}

/* The libc names: the libraries call free (and malloc) directly as well as through
   the table, and newlib's would need reentrancy data and a system break. */
void *malloc(size_t n) { return nw_malloc(n); }
void *calloc(size_t count, size_t size) { return nw_calloc(count, size); }
void *realloc(void *p, size_t n) { return nw_realloc(p, n); }
void free(void *p) { nw_free(p); }

uint32_t nw_heapFree(void)
{
    rtos_heapStats_t st;
    rtos_heapStats(&st);
    return (uint32_t)st.free;
}


/*
 * Time
 */

int64_t nw_timeUs(void)
{
    rtos_port_runtime_t t;
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    t = rtos_port_getRunTimeCounter();
    rtos_port_exitCritical(cs);
    return (int64_t)(t / (F_CPU / 1000000ul));
}

bool nw_inIsr(void)
{
    return _isrDepth != 0;
}


/*
 * Interrupts
 */

static void isrTrampoline(void)
{
    uint32_t pending = rsrInterrupt() & rsrIntenable() & _isrMask;
    while(pending != 0)
    {
        const unsigned n = (unsigned)__builtin_ctz(pending);
        pending &= pending - 1u;
        ++ _statIsrs;
        _diagLastIsrUs = rtos_getTimeUs();
        if(_isr[n].fn != NULL)
        {
            ++ _isrDepth;
            _isr[n].fn(_isr[n].arg);
            -- _isrDepth;
        }
    }
}

static void setIntr(int32_t cpuNo, uint32_t source, uint32_t num, int32_t prio)
{
    (void)cpuNo; (void)prio;
    NW_LOGD(TAG, "set_intr source %lu -> cpu int %lu", (unsigned long)source, (unsigned long)num);
    if(num < 32)
    {
        _isr[num].source = (uint8_t)source;
        *(volatile uint32_t *)(0x3FF00104ul + 4ul * source) = num;      /* DPORT_PRO_INTR_MAP */
    }
}

static void clearIntr(uint32_t source, uint32_t num)
{
    (void)source; (void)num;
}

static void setIsr(int32_t n, void *f, void *arg)
{
    rtos_port_cs_t cs;
    if(n < 0  ||  n >= 32)
        return;
    cs = rtos_port_enterCritical();
    _isr[n].fn = (void (*)(void *))(uintptr_t)f;
    _isr[n].arg = arg;
    _isrMask |= 1ul << n;
    rtos_port_exitCritical(cs);
    rtos_port_attachInterrupt((uint8_t)n, RTOS_PORT_SRC_NONE, isrTrampoline);
}

static void intsOn(uint32_t mask)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    wsrIntenable(rsrIntenable() | mask);
    rtos_port_exitCritical(cs);
}

static void intsOff(uint32_t mask)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    wsrIntenable(rsrIntenable() & ~mask);
    rtos_port_exitCritical(cs);
}

static bool isFromIsr(void)
{
    return _isrDepth != 0;
}

static void *spinLockCreate(void)
{
    return nw_malloc(4);
}

static uint32_t wifiIntDisable(void *mux)
{
    (void)mux;
    return (uint32_t)rtos_port_enterCritical();
}

static void wifiIntRestore(void *mux, uint32_t tmp)
{
    (void)mux;
    rtos_port_exitCritical((rtos_port_cs_t)tmp);
}

static void taskYieldFromIsr(void)
{
    /* nornOS reschedules after every interrupt. */
}


/*
 * Semaphores and mutexes
 */

static void *semphrCreate(uint32_t max, uint32_t init)
{
    rtos_sem_t *pS = nw_malloc(sizeof(*pS));
    if(pS != NULL)
        rtos_semInitMax(pS, (uint8_t)init, max > 255u ? 255u : (uint8_t)max);
    return pS;
}

static void semphrDelete(void *semphr)
{
    nw_free(semphr);
}

static int32_t semphrTake(void *semphr, uint32_t blockTimeTick)
{
    return rtos_semTake(semphr, blockTimeTick == OSI_FUNCS_TIME_BLOCKING ? RTOS_FOREVER : (rtos_tick_t)blockTimeTick) ? PD_PASS : PD_FAIL;
}

static int32_t semphrGive(void *semphr)
{
    if(_isrDepth != 0)
        rtos_semGiveFromIsr(semphr);
    else
        rtos_semGive(semphr);
    return PD_PASS;
}

static void *wifiThreadSemphrGet(void)
{
    const uint8_t id = rtos_getCurrentTask();
    if(!_threadSemReady[id])
    {
        rtos_semInitMax(&_threadSem[id], 0, 1);
        _threadSemReady[id] = true;
    }
    return &_threadSem[id];
}

static void *mutexCreateCommon(bool recursive)
{
    nwMutex_t *pM = nw_malloc(sizeof(*pM));
    if(pM != NULL)
    {
        rtos_semInitMax(&pM->sem, 1, 1);
        pM->owner = RTOS_NO_TASK;
        pM->depth = 0;
        pM->recursive = recursive;
    }
    return pM;
}

static void *mutexCreate(void) { return mutexCreateCommon(false); }
static void *recursiveMutexCreate(void) { return mutexCreateCommon(true); }

static void mutexDelete(void *mutex)
{
    nw_free(mutex);
}

static int32_t mutexLock(void *mutex)
{
    nwMutex_t *pM = mutex;
    const uint8_t me = rtos_getCurrentTask();
    if(pM->recursive  &&  pM->owner == me)
    {
        ++ pM->depth;
        return PD_PASS;
    }
    rtos_semTake(&pM->sem, RTOS_FOREVER);
    pM->owner = me;
    pM->depth = 1;
    return PD_PASS;
}

static int32_t mutexUnlock(void *mutex)
{
    nwMutex_t *pM = mutex;
    if(pM->owner != rtos_getCurrentTask())
    {
        NW_LOGW(TAG, "mutex unlock by non owner");
        return PD_FAIL;
    }
    if(--pM->depth == 0)
    {
        pM->owner = RTOS_NO_TASK;
        rtos_semGive(&pM->sem);
    }
    return PD_PASS;
}


/*
 * Queues
 */

static void *queueCreate(uint32_t queueLen, uint32_t itemSize)
{
    nwQueue_t *pQ;
    if(queueLen == 0  ||  queueLen > 255u)
    {
        NW_LOGE(TAG, "queue of %lu items not supported", (unsigned long)queueLen);
        return NULL;
    }
    pQ = nw_calloc(1, sizeof(*pQ) + queueLen * itemSize);
    if(pQ == NULL)
        return NULL;
    pQ->buf = (uint8_t *)(pQ + 1);
    pQ->itemSize = itemSize;
    pQ->cap = queueLen;
    rtos_semInitMax(&pQ->items, 0, (uint8_t)queueLen);
    if(_diagNumQueues < sizeof(_diagQueues) / sizeof(_diagQueues[0]))
        _diagQueues[_diagNumQueues++] = pQ;
    return pQ;
}

static void queueDelete(void *queue)
{
    nw_free(queue);
}

/** Insert at the tail or the head; waits up to the timeout for room by polling, which
    the library only needs when it has queued more than its own design allows. */
static int32_t queueSendCommon(nwQueue_t *pQ, const void *item, uint32_t blockTimeTick, bool front)
{
    rtos_tick_t left = blockTimeTick == OSI_FUNCS_TIME_BLOCKING ? RTOS_FOREVER : (rtos_tick_t)blockTimeTick;
    for(;;)
    {
        rtos_port_cs_t cs = rtos_port_enterCritical();
        if(pQ->count < pQ->cap)
        {
            uint32_t slot;
            if(front)
            {
                pQ->head = (pQ->head + pQ->cap - 1u) % pQ->cap;
                slot = pQ->head;
            }
            else
            {
                slot = pQ->tail;
                pQ->tail = (pQ->tail + 1u) % pQ->cap;
            }
            memcpy(pQ->buf + slot * pQ->itemSize, item, pQ->itemSize);
            ++ pQ->count;
            if(_isrDepth != 0)
            {
                if(pQ->count > 1)
                    ++ _diagBacklog;
                else if(!_diagPostPending)
                {
                    _diagPostPending = 1;
                    _diagPostUs = rtos_getTimeUs();
                }
            }
            rtos_port_exitCritical(cs);
            if(_isrDepth != 0)
                rtos_semGiveFromIsr(&pQ->items);
            else
                rtos_semGive(&pQ->items);
            return PD_PASS;
        }
        rtos_port_exitCritical(cs);
        if(left == 0  ||  _isrDepth != 0)
        {
            ++ pQ->dropped;
            return PD_FAIL;
        }
        rtos_delay(1);
        if(left != RTOS_FOREVER)
            -- left;
    }
}

static int32_t queueSend(void *queue, void *item, uint32_t blockTimeTick)
{
    return queueSendCommon(queue, item, blockTimeTick, false);
}

static int32_t queueSendFromIsr(void *queue, void *item, void *hptw)
{
    if(hptw != NULL)
        *(int *)hptw = 0;
    return queueSendCommon(queue, item, 0, false);
}

static int32_t queueSendToBack(void *queue, void *item, uint32_t blockTimeTick)
{
    return queueSendCommon(queue, item, blockTimeTick, false);
}

static int32_t queueSendToFront(void *queue, void *item, uint32_t blockTimeTick)
{
    return queueSendCommon(queue, item, blockTimeTick, true);
}

static int32_t queueRecv(void *queue, void *item, uint32_t blockTimeTick)
{
    nwQueue_t *pQ = queue;
    rtos_port_cs_t cs;
    if(!rtos_semTake(&pQ->items, blockTimeTick == OSI_FUNCS_TIME_BLOCKING ? RTOS_FOREVER : (rtos_tick_t)blockTimeTick))
        return PD_FAIL;
    cs = rtos_port_enterCritical();
    if(_diagPostPending)
    {
        const uint32_t lat = (uint32_t)(rtos_getTimeUs() - _diagPostUs);
        if(lat > _diagMaxWakeUs)
            _diagMaxWakeUs = lat;
        _diagPostPending = 0;
    }
    RTOS_ASSERT(pQ->count > 0);
    memcpy(item, pQ->buf + pQ->head * pQ->itemSize, pQ->itemSize);
    pQ->head = (pQ->head + 1u) % pQ->cap;
    -- pQ->count;
    rtos_port_exitCritical(cs);
    return PD_PASS;
}

static uint32_t queueMsgWaiting(void *queue)
{
    return ((nwQueue_t *)queue)->count;
}

static void *wifiCreateQueue(int queueLen, int itemSize)
{
    wifi_static_queue_t *pW = nw_calloc(1, sizeof(*pW));
    if(pW == NULL)
        return NULL;
    pW->handle = queueCreate((uint32_t)queueLen, (uint32_t)itemSize);
    if(pW->handle == NULL)
    {
        nw_free(pW);
        return NULL;
    }
    return pW;
}

static void wifiDeleteQueue(void *queue)
{
    wifi_static_queue_t *pW = queue;
    if(pW != NULL)
    {
        queueDelete(pW->handle);
        nw_free(pW);
    }
}


/*
 * Event groups
 */

static void *eventGroupCreate(void)
{
    rtos_evgroup_t *pG = nw_malloc(sizeof(*pG));
    if(pG != NULL)
        rtos_evgroupInit(pG, 0);
    return pG;
}

static void eventGroupDelete(void *event)
{
    nw_free(event);
}

static uint32_t eventGroupSetBits(void *event, uint32_t bits)
{
    if(_isrDepth != 0)
        rtos_evgroupSetFromIsr(event, (rtos_evt_t)bits);
    else
        rtos_evgroupSet(event, (rtos_evt_t)bits);
    return rtos_evgroupGet(event);
}

static uint32_t eventGroupClearBits(void *event, uint32_t bits)
{
    const uint32_t before = rtos_evgroupGet(event);
    rtos_evgroupClear(event, (rtos_evt_t)bits);
    return before;
}

static uint32_t eventGroupWaitBits(void *event, uint32_t bitsToWaitFor, int clearOnExit, int waitForAllBits, uint32_t blockTimeTick)
{
    const uint8_t opt = (uint8_t)((waitForAllBits ? RTOS_WAIT_ALL : RTOS_WAIT_ANY) | (clearOnExit ? RTOS_WAIT_CLEAR : 0u));
    return rtos_evgroupWait(event, (rtos_evt_t)bitsToWaitFor, opt,
                            blockTimeTick == OSI_FUNCS_TIME_BLOCKING ? RTOS_FOREVER : (rtos_tick_t)blockTimeTick);
}


/*
 * Tasks
 */

static void wifiTaskBody(void *arg)
{
    (void)arg;
    for(;;)
    {
        rtos_notifyTake(RTOS_FOREVER);
        if(_wifiFn != NULL)
        {
            NW_LOGD(TAG, "radio task running");
            _wifiFn(_wifiArg);
            NW_LOGW(TAG, "radio task returned");
            _wifiFn = NULL;
        }
    }
}

static int32_t taskCreate(void *taskFunc, const char *name, uint32_t stackDepth, void *param, uint32_t prio, void *taskHandle)
{
    NW_LOGD(TAG, "task_create '%s' stack %lu prio %lu", name, (unsigned long)stackDepth, (unsigned long)prio);
    if(_wifiFn != NULL)
    {
        NW_LOGE(TAG, "only one library task is supported ('%s' refused)", name);
        return PD_FAIL;
    }
    if(stackDepth > NW_CFG_WIFI_STACK)
        NW_LOGW(TAG, "task '%s' wants %lu bytes of stack, NW_CFG_WIFI_STACK is %u", name, (unsigned long)stackDepth, NW_CFG_WIFI_STACK);
    _wifiArg = param;
    _wifiFn = (void (*)(void *))(uintptr_t)taskFunc;
    if(taskHandle != NULL)
        *(void **)taskHandle = &_wifiHandle;
    rtos_notify(NW_CFG_TASK_WIFI, 1, RTOS_NOTIFY_INC);
    return PD_PASS;
}

static int32_t taskCreatePinnedToCore(void *taskFunc, const char *name, uint32_t stackDepth, void *param, uint32_t prio, void *taskHandle, uint32_t coreId)
{
    (void)coreId;
    return taskCreate(taskFunc, name, stackDepth, param, prio, taskHandle);
}

static void taskDelete(void *taskHandle)
{
    NW_LOGW(TAG, "task_delete %p: tasks are static in nornOS, the task parks itself", taskHandle);
    if(taskHandle == &_wifiHandle  &&  rtos_getCurrentTask() == NW_CFG_TASK_WIFI)
    {
        _wifiFn = NULL;
        for(;;)
            rtos_notifyTake(RTOS_FOREVER);
    }
}

static void taskDelay(uint32_t tick)
{
    rtos_delay((rtos_tick_t)tick);
}

static int32_t taskMsToTick(uint32_t ms)
{
    return (int32_t)MS_TO_TICKS(ms);
}

static void *taskGetCurrentTask(void)
{
    return rtos_getCurrentTask() == NW_CFG_TASK_WIFI ? (void *)&_wifiHandle : (void *)(uintptr_t)(0x100u + rtos_getCurrentTask());
}

static int32_t taskGetMaxPriority(void)
{
    return RTOS_CFG_NUM_TASKS;
}


/*
 * Timers: the nornOS timer fires in the tick and queues the record for the service
 * task, which calls the library's function in task context like ESP-IDF's esp_timer.
 */

static void timerFired(void *arg)
{
    nwTimer_t *p = arg;
    if(!p->pending)
    {
        p->pending = 1;
        p->pNextPending = NULL;
        if(_pendingTail != NULL)
            _pendingTail->pNextPending = p;
        else
            _pendingHead = p;
        _pendingTail = p;
    }
    rtos_notifyFromIsr(NW_CFG_TASK_SERVICE, EV_TIMER, RTOS_NOTIFY_OR);
}

static void unqueueTimer(nwTimer_t *p)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    if(p->pending)
    {
        nwTimer_t **pp = &_pendingHead, *prev = NULL;
        while(*pp != NULL  &&  *pp != p)
        {
            prev = *pp;
            pp = &(*pp)->pNextPending;
        }
        if(*pp == p)
        {
            *pp = p->pNextPending;
            if(_pendingTail == p)
                _pendingTail = prev;
        }
        p->pending = 0;
    }
    rtos_port_exitCritical(cs);
}

static void runTimers(void)
{
    for(;;)
    {
        nwTimer_t *p;
        rtos_port_cs_t cs = rtos_port_enterCritical();
        p = _pendingHead;
        if(p != NULL)
        {
            _pendingHead = p->pNextPending;
            if(_pendingHead == NULL)
                _pendingTail = NULL;
            p->pending = 0;
        }
        rtos_port_exitCritical(cs);
        if(p == NULL)
            return;
        ++ _statTimerCallbacks;
        if(p->fn != NULL)
            p->fn(p->arg);
    }
}

static nwTimer_t *timerOf(void *ptimer)
{
    etsTimer_t *pT = ptimer;
    return pT->timer_period == TIMER_MAGIC ? (nwTimer_t *)pT->timer_expire : NULL;
}

static void timerSetfn(void *ptimer, void *pfunction, void *parg)
{
    etsTimer_t *pT = ptimer;
    nwTimer_t *p = timerOf(ptimer);
    if(p == NULL)
    {
        p = nw_calloc(1, sizeof(*p));
        if(p == NULL)
            return;
        rtos_timerInit(&p->t, timerFired, p);
        pT->timer_expire = (uint32_t)p;
        pT->timer_period = TIMER_MAGIC;
        pT->timer_next = NULL;
    }
    p->fn = (void (*)(void *))(uintptr_t)pfunction;
    p->arg = parg;
    pT->timer_func = p->fn;
    pT->timer_arg = parg;
}

static void timerArmTicks(void *ptimer, rtos_tick_t ticks, bool repeat)
{
    nwTimer_t *p = timerOf(ptimer);
    if(p == NULL)
    {
        NW_LOGE(TAG, "timer armed before setfn");
        return;
    }
    if(ticks == 0)
        ticks = 1;
    unqueueTimer(p);
    rtos_timerStart(&p->t, ticks, repeat ? ticks : 0);
}

static void timerArm(void *timer, uint32_t tmout, bool repeat)
{
    timerArmTicks(timer, MS_TO_TICKS(tmout), repeat);
}

static void timerArmUs(void *ptimer, uint32_t us, bool repeat)
{
    timerArmTicks(ptimer, MS_TO_TICKS((us + 999ul) / 1000ul), repeat);
}

static void timerDisarm(void *timer)
{
    nwTimer_t *p = timerOf(timer);
    if(p != NULL)
    {
        rtos_timerStop(&p->t);
        unqueueTimer(p);
    }
}

void nw_osi_timerInit(nwTimer_t *p, void (*fn)(void *), void *arg)
{
    memset(p, 0, sizeof(*p));
    rtos_timerInit(&p->t, timerFired, p);
    p->fn = fn;
    p->arg = arg;
}

void nw_osi_timerStart(nwTimer_t *p, uint32_t ms)
{
    rtos_tick_t ticks = MS_TO_TICKS(ms);
    unqueueTimer(p);
    rtos_timerStart(&p->t, ticks != 0 ? ticks : 1, 0);
}

void nw_osi_timerStop(nwTimer_t *p)
{
    rtos_timerStop(&p->t);
    unqueueTimer(p);
}

static void timerDone(void *ptimer)
{
    etsTimer_t *pT = ptimer;
    nwTimer_t *p = timerOf(ptimer);
    if(p != NULL)
    {
        rtos_timerStop(&p->t);
        unqueueTimer(p);
        nw_free(p);
        pT->timer_expire = 0;
        pT->timer_period = 0;
    }
}


/*
 * Events: copied into a ring for the service task, which hands them to nw_wifi_event.
 */

static int32_t eventPost(const char *eventBase, int32_t eventId, void *eventData, size_t eventDataSize, uint32_t ticksToWait)
{
    rtos_port_cs_t cs;
    (void)ticksToWait;
    if(eventBase != WIFI_EVENT  &&  !(eventBase != NULL  &&  strcmp(eventBase, "WIFI_EVENT") == 0))
    {
        NW_LOGD(TAG, "event base %s id %ld ignored", eventBase != NULL ? eventBase : "?", (long)eventId);
        return ESP_OK;
    }
    cs = rtos_port_enterCritical();
    if(_evCount >= NW_CFG_EVENT_QUEUE)
    {
        rtos_port_exitCritical(cs);
        ++ _statEventsDropped;
        NW_LOGW(TAG, "event %ld dropped, queue full", (long)eventId);
        return ESP_FAIL;
    }
    {
        event_t *pE = &_events[(_evHead + _evCount) % NW_CFG_EVENT_QUEUE];
        pE->id = eventId;
        pE->size = (uint16_t)(eventDataSize > NW_CFG_EVENT_DATA_MAX ? NW_CFG_EVENT_DATA_MAX : eventDataSize);
        if(pE->size != 0  &&  eventData != NULL)
            memcpy(pE->data, eventData, pE->size);
        else
            pE->size = 0;
        ++ _evCount;
    }
    rtos_port_exitCritical(cs);
    ++ _statEvents;
    if(_isrDepth != 0)
        rtos_notifyFromIsr(NW_CFG_TASK_SERVICE, EV_EVENT, RTOS_NOTIFY_OR);
    else
        rtos_notify(NW_CFG_TASK_SERVICE, EV_EVENT, RTOS_NOTIFY_OR);
    return ESP_OK;
}

static void runEvents(void)
{
    for(;;)
    {
        event_t ev;
        rtos_port_cs_t cs = rtos_port_enterCritical();
        if(_evCount == 0)
        {
            rtos_port_exitCritical(cs);
            return;
        }
        ev = _events[_evHead];
        _evHead = (uint8_t)((_evHead + 1u) % NW_CFG_EVENT_QUEUE);
        -- _evCount;
        rtos_port_exitCritical(cs);
        nw_wifi_event(ev.id, ev.size != 0 ? ev.data : NULL, ev.size);
    }
}

static void serviceTask(void *arg)
{
    (void)arg;
    for(;;)
    {
        uint16_t v = 0;
        rtos_notifyWait(&v, RTOS_FOREVER);
        if(v & EV_TIMER)
            runTimers();
        if(v & EV_EVENT)
            runEvents();
    }
}


/*
 * The rest of the table
 */

static bool envIsChip(void) { return true; }
static void nop(void) { }
static int phyUpdateCountryInfo(const char *country) { (void)country; return 0; }
static int readMac(uint8_t *mac, unsigned int type) { return nw_readMac(mac, type); }
static uint32_t getFreeHeapSize(void) { return nw_heapFree(); }
static uint32_t rand32(void) { return nw_random(); }
static unsigned long randomUl(void) { return nw_random(); }
static int getRandom(uint8_t *buf, size_t len)
{
    size_t i;
    for(i=0; i<len; ++i)
        buf[i] = (uint8_t)nw_random();
    return 0;
}
static int getTime(void *t)
{
    struct { int32_t sec; int32_t usec; } *pT = t;
    const int64_t us = nw_timeUs();
    pT->sec = (int32_t)(us / 1000000);
    pT->usec = (int32_t)(us % 1000000);
    return 0;
}
static int64_t espTimerGetTime(void) { return nw_timeUs(); }

/* NVS is not used (nvs_enable 0 in the init configuration); these answer "not found". */
#define ESP_ERR_NVS_NOT_FOUND   0x1102
static int nvsSetI8(uint32_t h, const char *k, int8_t v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsGetI8(uint32_t h, const char *k, int8_t *v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsSetU8(uint32_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsGetU8(uint32_t h, const char *k, uint8_t *v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsSetU16(uint32_t h, const char *k, uint16_t v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsGetU16(uint32_t h, const char *k, uint16_t *v) { (void)h; (void)k; (void)v; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsOpen(const char *n, unsigned int m, uint32_t *h) { (void)n; (void)m; (void)h; return ESP_ERR_NVS_NOT_FOUND; }
static void nvsClose(uint32_t h) { (void)h; }
static int nvsCommit(uint32_t h) { (void)h; return ESP_OK; }
static int nvsSetBlob(uint32_t h, const char *k, const void *v, size_t l) { (void)h; (void)k; (void)v; (void)l; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsGetBlob(uint32_t h, const char *k, void *v, size_t *l) { (void)h; (void)k; (void)v; (void)l; return ESP_ERR_NVS_NOT_FOUND; }
static int nvsEraseKey(uint32_t h, const char *k) { (void)h; (void)k; return ESP_ERR_NVS_NOT_FOUND; }

/* Coexistence with Bluetooth: not built, as ESP-IDF without CONFIG_SW_COEXIST_ENABLE. */
static int coexInit(void) { return 0; }
static void coexDeinit(void) { }
static int coexEnable(void) { return 0; }
static void coexDisable(void) { }
static uint32_t coexStatusGet(void) { return 0; }
static void coexConditionSet(uint32_t type, bool dissatisfy) { (void)type; (void)dissatisfy; }
static int coexWifiRequest(uint32_t event, uint32_t latency, uint32_t duration) { (void)event; (void)latency; (void)duration; return 0; }
static int coexWifiRelease(uint32_t event) { (void)event; return 0; }
static int coexWifiChannelSet(uint8_t primary, uint8_t secondary) { (void)primary; (void)secondary; return 0; }
static int coexEventDurationGet(uint32_t event, uint32_t *duration) { (void)event; (void)duration; return 0; }
static int coexPtiGet(uint32_t event, uint8_t *pti) { (void)event; (void)pti; return 0; }
static void coexSchmStatusBitClear(uint32_t type, uint32_t status) { (void)type; (void)status; }
static void coexSchmStatusBitSet(uint32_t type, uint32_t status) { (void)type; (void)status; }
static int coexSchmIntervalSet(uint32_t interval) { (void)interval; return 0; }
static uint32_t coexSchmIntervalGet(void) { return 0; }
static uint8_t coexSchmCurrPeriodGet(void) { return 0; }
static void *coexSchmCurrPhaseGet(void) { return NULL; }
static int coexSchmProcessRestart(void) { return 0; }
static int coexSchmRegisterCb(int type, int (*cb)(int)) { (void)type; (void)cb; return 0; }
static int coexRegisterStartCb(int (*cb)(void)) { (void)cb; return 0; }
static int coexSchmFlexiblePeriodSet(uint8_t period) { (void)period; return 0; }
static uint8_t coexSchmFlexiblePeriodGet(void) { return 1; }
static void *coexSchmGetPhaseByIdx(int idx) { (void)idx; return NULL; }

static void *mallocWrap(size_t n) { return nw_malloc(n); }
static void freeWrap(void *p) { nw_free(p); }
static void *reallocWrap(void *p, size_t n) { return nw_realloc(p, n); }
static void *callocWrap(size_t count, size_t size) { return nw_calloc(count, size); }
static void *zallocWrap(size_t n) { return nw_calloc(1, n); }
static void spinLockDelete(void *p) { nw_free(p); }
static void logWrite(unsigned int level, const char *tag, const char *format, ...)
{
    va_list ap;
    (void)tag;
    va_start(ap, format);
    nw_logRawv((int)level, format, ap);
    va_end(ap);
}
static void logWritev(unsigned int level, const char *tag, const char *format, va_list args) { (void)tag; nw_logRawv((int)level, format, args); }
static uint32_t logTimestamp(void) { return (uint32_t)(rtos_getTick() * 1000ul / RTOS_CFG_TICK_HZ); }

wifi_osi_funcs_t g_wifi_osi_funcs =
{
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = envIsChip,
    ._set_intr = setIntr,
    ._clear_intr = clearIntr,
    ._set_isr = setIsr,
    ._ints_on = intsOn,
    ._ints_off = intsOff,
    ._is_from_isr = isFromIsr,
    ._spin_lock_create = spinLockCreate,
    ._spin_lock_delete = spinLockDelete,
    ._wifi_int_disable = wifiIntDisable,
    ._wifi_int_restore = wifiIntRestore,
    ._task_yield_from_isr = taskYieldFromIsr,
    ._semphr_create = semphrCreate,
    ._semphr_delete = semphrDelete,
    ._semphr_take = semphrTake,
    ._semphr_give = semphrGive,
    ._wifi_thread_semphr_get = wifiThreadSemphrGet,
    ._mutex_create = mutexCreate,
    ._recursive_mutex_create = recursiveMutexCreate,
    ._mutex_delete = mutexDelete,
    ._mutex_lock = mutexLock,
    ._mutex_unlock = mutexUnlock,
    ._queue_create = queueCreate,
    ._queue_delete = queueDelete,
    ._queue_send = queueSend,
    ._queue_send_from_isr = queueSendFromIsr,
    ._queue_send_to_back = queueSendToBack,
    ._queue_send_to_front = queueSendToFront,
    ._queue_recv = queueRecv,
    ._queue_msg_waiting = queueMsgWaiting,
    ._event_group_create = eventGroupCreate,
    ._event_group_delete = eventGroupDelete,
    ._event_group_set_bits = eventGroupSetBits,
    ._event_group_clear_bits = eventGroupClearBits,
    ._event_group_wait_bits = eventGroupWaitBits,
    ._task_create_pinned_to_core = taskCreatePinnedToCore,
    ._task_create = taskCreate,
    ._task_delete = taskDelete,
    ._task_delay = taskDelay,
    ._task_ms_to_tick = taskMsToTick,
    ._task_get_current_task = taskGetCurrentTask,
    ._task_get_max_priority = taskGetMaxPriority,
    ._malloc = mallocWrap,
    ._free = freeWrap,
    ._event_post = eventPost,
    ._get_free_heap_size = getFreeHeapSize,
    ._rand = rand32,
    ._dport_access_stall_other_cpu_start_wrap = nop,
    ._dport_access_stall_other_cpu_end_wrap = nop,
    ._wifi_apb80m_request = nop,
    ._wifi_apb80m_release = nop,
    ._phy_disable = nw_phyDisable,
    ._phy_enable = nw_phyEnable,
    ._phy_common_clock_enable = nw_phyCommonClockEnable,
    ._phy_common_clock_disable = nw_phyCommonClockDisable,
    ._phy_update_country_info = phyUpdateCountryInfo,
    ._read_mac = readMac,
    ._timer_arm = timerArm,
    ._timer_disarm = timerDisarm,
    ._timer_done = timerDone,
    ._timer_setfn = timerSetfn,
    ._timer_arm_us = timerArmUs,
    ._wifi_reset_mac = nw_wifiResetMac,
    ._wifi_clock_enable = nw_wifiClockEnable,
    ._wifi_clock_disable = nw_wifiClockDisable,
    ._wifi_rtc_enable_iso = nop,
    ._wifi_rtc_disable_iso = nop,
    ._esp_timer_get_time = espTimerGetTime,
    ._nvs_set_i8 = nvsSetI8,
    ._nvs_get_i8 = nvsGetI8,
    ._nvs_set_u8 = nvsSetU8,
    ._nvs_get_u8 = nvsGetU8,
    ._nvs_set_u16 = nvsSetU16,
    ._nvs_get_u16 = nvsGetU16,
    ._nvs_open = nvsOpen,
    ._nvs_close = nvsClose,
    ._nvs_commit = nvsCommit,
    ._nvs_set_blob = nvsSetBlob,
    ._nvs_get_blob = nvsGetBlob,
    ._nvs_erase_key = nvsEraseKey,
    ._get_random = getRandom,
    ._get_time = getTime,
    ._random = randomUl,
    ._log_write = logWrite,
    ._log_writev = logWritev,
    ._log_timestamp = logTimestamp,
    ._malloc_internal = mallocWrap,
    ._realloc_internal = reallocWrap,
    ._calloc_internal = callocWrap,
    ._zalloc_internal = zallocWrap,
    ._wifi_malloc = mallocWrap,
    ._wifi_realloc = reallocWrap,
    ._wifi_calloc = callocWrap,
    ._wifi_zalloc = zallocWrap,
    ._wifi_create_queue = wifiCreateQueue,
    ._wifi_delete_queue = wifiDeleteQueue,
    ._coex_init = coexInit,
    ._coex_deinit = coexDeinit,
    ._coex_enable = coexEnable,
    ._coex_disable = coexDisable,
    ._coex_status_get = coexStatusGet,
    ._coex_condition_set = coexConditionSet,
    ._coex_wifi_request = coexWifiRequest,
    ._coex_wifi_release = coexWifiRelease,
    ._coex_wifi_channel_set = coexWifiChannelSet,
    ._coex_event_duration_get = coexEventDurationGet,
    ._coex_pti_get = coexPtiGet,
    ._coex_schm_status_bit_clear = coexSchmStatusBitClear,
    ._coex_schm_status_bit_set = coexSchmStatusBitSet,
    ._coex_schm_interval_set = coexSchmIntervalSet,
    ._coex_schm_interval_get = coexSchmIntervalGet,
    ._coex_schm_curr_period_get = coexSchmCurrPeriodGet,
    ._coex_schm_curr_phase_get = coexSchmCurrPhaseGet,
    ._coex_schm_process_restart = coexSchmProcessRestart,
    ._coex_schm_register_cb = coexSchmRegisterCb,
    ._coex_register_start_cb = coexRegisterStartCb,
    ._coex_schm_flexible_period_set = coexSchmFlexiblePeriodSet,
    ._coex_schm_flexible_period_get = coexSchmFlexiblePeriodGet,
    ._coex_schm_get_phase_by_idx = coexSchmGetPhaseByIdx,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};


/*
 * Setup and statistics
 */

static void diagJitter(void *arg)
{
    const int64_t now = rtos_getTimeUs();
    (void)arg;
    if(_diagLastTickUs != 0)
    {
        const int64_t late = now - _diagLastTickUs - 1000;
        if(late > (int64_t)_diagTickMaxLateUs) _diagTickMaxLateUs = (uint32_t)late;
        if(late > 200) ++ _diagTicksLate200;
    }
    _diagLastTickUs = now;
}

uint32_t nw_osi_diagTickLate(uint32_t *pCount)
{
    const uint32_t m = _diagTickMaxLateUs;
    *pCount = _diagTicksLate200;
    _diagTickMaxLateUs = 0; _diagTicksLate200 = 0;
    return m;
}

static void diagTick(void *arg)
{
    const int64_t now = rtos_getTimeUs();
    (void)arg;
    if(++_diagTicks == 10)
    {
        _diagTicks = 0;
        if(_diagSec < sizeof(_diagIsrsPerSec) / sizeof(_diagIsrsPerSec[0]))
            _diagIsrsPerSec[_diagSec++] = (uint16_t)(_statIsrs - _diagIsrsAtSec);
        _diagIsrsAtSec = _statIsrs;
    }
    if(_diagLastIsrUs != 0)
    {
        const uint32_t gapMs = (uint32_t)((now - _diagLastIsrUs) / 1000);
        if(gapMs >= 1000)
        {
            if(!_diagInGap)
            {
                _diagInGap = 1;
                ++ _diagGaps;
            }
            if(gapMs > _diagMaxGapMs)
            {
                _diagMaxGapMs = gapMs;
                _diagGapAtMs = (uint32_t)(now / 1000);
                _diagIntenableAtGap = rsrIntenable();
                _diagInterruptAtGap = rsrInterrupt();
            }
        }
        else
            _diagInGap = 0;
    }
}

extern volatile uint32_t nw_phyEnables, nw_phyDisables, nw_phyWakeups;
extern uint8_t temprature_sens_read(void);        /* libphy: raw temperature sensor code */
extern int esp_wifi_get_max_tx_power(int8_t *power);

void nw_osi_diagDump(void)
{
    unsigned i;
    {
        int8_t txp = 0;
        (void)esp_wifi_get_max_tx_power(&txp);
        nw_log(2, TAG, "diag: phy enable %lu disable %lu wakeup %lu, temperature code %u, max tx power %d (quarter dBm)",
               (unsigned long)nw_phyEnables, (unsigned long)nw_phyDisables, (unsigned long)nw_phyWakeups,
               (unsigned)temprature_sens_read(), (int)txp);
    }
    nw_log(2, TAG, "diag: isrs %lu, %lu pauses >= 1 s, longest %lu ms ending at %lu ms (intenable %08lx interrupt %08lx), "
                   "wake max %lu us, backlog %lu",
           (unsigned long)_statIsrs, (unsigned long)_diagGaps, (unsigned long)_diagMaxGapMs, (unsigned long)_diagGapAtMs,
           (unsigned long)_diagIntenableAtGap, (unsigned long)_diagInterruptAtGap, (unsigned long)_diagMaxWakeUs,
           (unsigned long)_diagBacklog);
    {
        char line[200];
        unsigned k = 0;
        for(i=0; i<_diagSec && k<sizeof(line)-8; ++i)
            k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " %u", (unsigned)_diagIsrsPerSec[i]);
        nw_log(2, TAG, "diag: isrs per second:%s", line);
    }
    for(i=0; i<_diagNumQueues; ++i)
    {
        const nwQueue_t *pQ = _diagQueues[i];
        nw_log(2, TAG, "diag: queue %u cap %lu item %lu count %lu dropped %lu sem count %u lost %u",
               i, (unsigned long)pQ->cap, (unsigned long)pQ->itemSize, (unsigned long)pQ->count,
               (unsigned long)pQ->dropped, (unsigned)pQ->items.count, (unsigned)pQ->items.lost);
    }
}

void nw_osi_init(void)
{
    rtos_createTask(NW_CFG_TASK_WIFI, wifiTaskBody, NULL, _stackWifi, sizeof(_stackWifi));
    rtos_createTask(NW_CFG_TASK_SERVICE, serviceTask, NULL, _stackService, sizeof(_stackService));
    nw_printf_init();
    rtos_timerInit(&_diagTimer, diagTick, NULL);
    rtos_timerStart(&_diagTimer, RTOS_MS(100), RTOS_MS(100));
    rtos_timerInit(&_diagJitterTimer, diagJitter, NULL);
    rtos_timerStart(&_diagJitterTimer, 1, 1);
}

void nw_osi_getStats(uint32_t *pEvents, uint32_t *pEventsDropped, uint32_t *pTimerCallbacks, uint32_t *pIsrs)
{
    *pEvents = _statEvents;
    *pEventsDropped = _statEventsDropped;
    *pTimerCallbacks = _statTimerCallbacks;
    *pIsrs = _statIsrs;
}

/* Used by the libraries directly. */
uint32_t esp_dport_access_reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

/*
 * libc and ESP-IDF functions the supplicant's OS port calls
 */

#include <sys/time.h>

int gettimeofday(struct timeval *tv, void *tz)
{
    const int64_t us = nw_timeUs();
    (void)tz;
    if(tv != NULL)
    {
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    rtos_delay(MS_TO_TICKS(seconds * 1000ul));
    return 0;
}

int usleep(useconds_t us)
{
    rtos_delay(MS_TO_TICKS((us + 999ul) / 1000ul));
    return 0;
}

uint32_t esp_random(void)
{
    return nw_random();
}

void esp_fill_random(void *buf, size_t len)
{
    getRandom(buf, len);
}

int64_t esp_timer_get_time(void)
{
    return nw_timeUs();
}

void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile uint8_t *p = buf;
    while(len-- != 0)
        *p++ = 0;
}

/* assert() and abort() of the supplicant end in a panic instead of newlib's stdio. */
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    (void)func;
    NW_LOGE(TAG, "assertion '%s' failed at %s:%d", expr != NULL ? expr : "?", file != NULL ? file : "?", line);
    rtos_panic(RTOS_PANIC_APPLICATION, 0x4E57u, (uint32_t)line);
}

void abort(void)
{
    rtos_panic(RTOS_PANIC_APPLICATION, 0x4E57u, 0xABu);
}

int *__errno(void)
{
    static int e;
    return &e;
}
