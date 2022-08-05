#include "mu.h"

#include <linux/futex.h>
#include <sched.h>
#include <stdatomic.h>
#define MUTEX_TYPE_MASK 0x000f
#define MUTEX_PROTOCOL_MASK 0x00f0
#define MUTEX_PRIOCEILING_MASK 0xff00
#define MUTEX_PROTOCOL_SHIFT 4
#define MUTEX_PRIOCEILING_SHIFT 8

/* Normal mutex */
static int lock_normal(muthread_mutex_t *mutex)
{  
    if (atomic_bool_cmpxchg(&mutex->futex, 0, 1))
        return 0;
    else {
        if (atomic_load_explicit(&mutex->futex, memory_order_relaxed) == 2)
            goto futex;

        while (atomic_exchange_explicit(&mutex->futex, 2, memory_order_acquire) != 0) {
            futex:
            SYSCALL3(__NR_futex, &mutex->futex, FUTEX_WAIT_PRIVATE, 2);
        }
    }
    return 0;
}

static int trylock_normal(muthread_mutex_t *mutex)
{
    if (atomic_bool_cmpxchg(&mutex->futex, 0, 1))
        return 0;
    return -EBUSY;
}

static int unlock_normal(muthread_mutex_t *mutex)
{
    if (atomic_exchange_explicit(&mutex->futex, 0, memory_order_release) == 2)
        SYSCALL3(__NR_futex, &mutex->futex, FUTEX_WAKE_PRIVATE, 1);
    return 0;
}

/* Errorcheck mutex */
static int lock_errorcheck(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    if (mutex->owner == self)
        return -EDEADLK;

    lock_normal(mutex);
    mutex->owner = self;
    return 0;
}

static int trylock_errorcheck(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    if (mutex->owner == self)
        return -EDEADLK;

    int ret = trylock_normal(mutex);
    if (ret == 0)
        mutex->owner = self;
    return ret;
}

static int unlock_errorcheck(muthread_mutex_t *mutex)
{
    if (mutex->owner != muthread_self() || mutex->futex == 0)
        return -EPERM;
    mutex->owner = 0;
    unlock_normal(mutex);
    return 0;
}

/* Recursive mutex */
static int lock_recursive(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    if (mutex->owner != self) {
        lock_normal(mutex);
        mutex->owner = self;
    }
    /* Check counter overflow */
    if (mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    ++mutex->counter;
    return 0;
}

static int trylock_recursive(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    if (mutex->owner != self && trylock_normal(mutex))
        return -EBUSY;

    if (mutex->owner != self) {
        mutex->owner = self;
    }
    /* Check counter overflow */
    if (mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    ++mutex->counter;
    return 0;
}

static int unlock_recursive(muthread_mutex_t *mutex)
{
    if (mutex->owner != muthread_self())
        return -EPERM;

    --mutex->counter;
    if (mutex->counter == 0) {
        mutex->owner = 0;
        unlock_normal(mutex);
    }
    return 0;
}

/* Priority inheritance mutex*/
static int lock_priority_inherit(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (mutex->owner == self && type == TBTHREAD_MUTEX_ERRORCHECK)
        return -EDEADLK;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    SYSCALL2(__NR_futex, mutex, FUTEX_LOCK_PI);
    mutex->owner = self;
    
    if (type == TBTHREAD_MUTEX_RECURSIVE)
        ++mutex->counter;
    return 0;
}

static int trylock_priority_inherit(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (mutex->owner == self && type == TBTHREAD_MUTEX_ERRORCHECK)
        return -EDEADLK;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    int ret = SYSCALL2(__NR_futex, mutex, FUTEX_TRYLOCK_PI);
    if (ret == 0) {
        mutex->owner = self;
        if (type == TBTHREAD_MUTEX_RECURSIVE)
            ++mutex->counter;
    }
    return ret;
}

static int unlock_priority_inherit(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (type == TBTHREAD_MUTEX_ERRORCHECK && 
        (mutex->owner != self || mutex->futex == 0))
        return -EPERM;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->owner != self)
        return -EPERM;

    if (type == TBTHREAD_MUTEX_RECURSIVE) {
        --mutex->counter;
        if (mutex->counter == 0) {
            mutex->owner = 0;
            SYSCALL2(__NR_futex, mutex, FUTEX_UNLOCK_PI);
        }
    } else { 
        mutex->owner = 0;
        SYSCALL2(__NR_futex, mutex, FUTEX_UNLOCK_PI);
    }
    return 0;
}

/* Priority protection mutex*/
static int lock_priority_protect(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (mutex->owner == self && type == TBTHREAD_MUTEX_ERRORCHECK)
        return -EDEADLK;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    lock_normal(mutex);
    mutex->owner = self;
    if (type == TBTHREAD_MUTEX_RECURSIVE)
        ++mutex->counter;
    int ceiling = (mutex->type & MUTEX_PRIOCEILING_MASK) >> MUTEX_PRIOCEILING_SHIFT;
    if (change_muthread_priority(self, ceiling, 0, 1) < 0)
        muprint("fail to change priority\n");
    return 0;
}

static int trylock_priority_protect(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (mutex->owner == self && type == TBTHREAD_MUTEX_ERRORCHECK)
        return -EDEADLK;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->counter == (uint64_t) -1)
        return -EAGAIN;

    int ret = trylock_normal(mutex);
    if (ret == 0) {
        mutex->owner = self;
        if (type == TBTHREAD_MUTEX_RECURSIVE)
            ++mutex->counter;
        int ceiling = (mutex->type & MUTEX_PRIOCEILING_MASK) >> MUTEX_PRIOCEILING_SHIFT;
        if (change_muthread_priority(self, ceiling, 0, 1) < 0)
            muprint("fail to change priority\n");
    }
    return ret;
}

static int unlock_priority_protect(muthread_mutex_t *mutex)
{
    muthread_t self = muthread_self();
    uint16_t type = mutex->type & MUTEX_TYPE_MASK;
    if (type == TBTHREAD_MUTEX_ERRORCHECK && 
        (mutex->owner != self || mutex->futex == 0))
        return -EPERM;
    if (type == TBTHREAD_MUTEX_RECURSIVE && mutex->owner != self)
        return -EPERM;

    if (type == TBTHREAD_MUTEX_RECURSIVE) {
        --mutex->counter;
        if (mutex->counter == 0) {
            mutex->owner = 0;
            unlock_normal(mutex);
        }
    } else {
        mutex->owner = 0;
        unlock_normal(mutex);
    }
    if (change_muthread_priority(self, (uint32_t) -1, 0, 0) < 0)
        muprint("fail to set priority to original\n");
    return 0;
}
/* Mutex function tables */
static int (*lockers[])(muthread_mutex_t *) = {
    lock_normal,
    lock_errorcheck,
    lock_recursive,
    lock_priority_inherit,
    lock_priority_protect,
};

static int (*trylockers[])(muthread_mutex_t *) = {
    trylock_normal,
    trylock_errorcheck,
    trylock_recursive,
    trylock_priority_inherit,
    trylock_priority_protect,
};

static int (*unlockers[])(muthread_mutex_t *) = {
    unlock_normal,
    unlock_errorcheck,
    unlock_recursive,
    unlock_priority_inherit,
    unlock_priority_protect,
};

/* Init attributes */
int muthread_mutexattr_init(muthread_mutexattr_t *attr)
{
    attr->type = TBTHREAD_MUTEX_DEFAULT;
    return 0;
}

/* Set attributes */
int muthread_mutexattr_settype(muthread_mutexattr_t *attr, int type)
{
    if (type < TBTHREAD_MUTEX_NORMAL || type > TBTHREAD_MUTEX_ERRORCHECK)
        return -EINVAL;
    attr->type = (attr->type & ~(MUTEX_TYPE_MASK)) | type;
    return 0;
}

int muthread_mutexattr_setprotocol(muthread_mutexattr_t *attr, int protocol)
{
    if (protocol != TBTHREAD_PRIO_NONE && protocol != TBTHREAD_PRIO_INHERIT
      && protocol != TBTHREAD_PRIO_PROTECT)
        return -EINVAL;
    attr->type = (attr->type & ~(MUTEX_PROTOCOL_MASK)) | (protocol << MUTEX_PROTOCOL_SHIFT);
    return 0;
}

int muthread_mutexattr_setprioceiling(muthread_mutexattr_t *attr, int prioceiling)
{
    uint16_t prio_max = 99, prio_min = 1;
    if (prioceiling > prio_max || prioceiling < prio_min)
        return -EINVAL;
    attr->type = (attr->type & ~(MUTEX_PRIOCEILING_MASK)) | (prioceiling << MUTEX_PRIOCEILING_SHIFT);
    return 0;
}

/* Initialize the mutex */
int muthread_mutex_init(muthread_mutex_t *mutex,
                        const muthread_mutexattr_t *attr)
{
    uint16_t type = TBTHREAD_MUTEX_DEFAULT;
    if (attr)
        type = attr->type;
    mutex->futex = 0;
    mutex->type = type;
    mutex->owner = 0;
    mutex->counter = 0;
    return 0;
}

/* Lock the mutex */
int muthread_mutex_lock(muthread_mutex_t *mutex)
{
    uint16_t type = (mutex->type & MUTEX_PROTOCOL_MASK) >> MUTEX_PROTOCOL_SHIFT;
    if(!type)
        type = mutex->type & MUTEX_TYPE_MASK;
    return (*lockers[type])(mutex);
}

/* Try locking the mutex */
int muthread_mutex_trylock(muthread_mutex_t *mutex)
{
    uint16_t type = (mutex->type & MUTEX_PROTOCOL_MASK) >> MUTEX_PROTOCOL_SHIFT;
    if(!type)
        type = mutex->type & MUTEX_TYPE_MASK;
    return (*trylockers[type])(mutex);
}

/* Unlock the mutex */
int muthread_mutex_unlock(muthread_mutex_t *mutex)
{
    uint16_t type = (mutex->type >> MUTEX_PROTOCOL_SHIFT) & (MUTEX_TYPE_MASK);
    if(!type)
        type = mutex->type & MUTEX_TYPE_MASK;
    return (*unlockers[type])(mutex);
}