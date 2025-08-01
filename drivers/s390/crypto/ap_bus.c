// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright IBM Corp. 2006, 2023
 * Author(s): Cornelia Huck <cornelia.huck@de.ibm.com>
 *	      Martin Schwidefsky <schwidefsky@de.ibm.com>
 *	      Ralph Wuerthner <rwuerthn@de.ibm.com>
 *	      Felix Beck <felix.beck@de.ibm.com>
 *	      Holger Dengler <hd@linux.vnet.ibm.com>
 *	      Harald Freudenberger <freude@linux.ibm.com>
 *
 * Adjunct processor bus.
 */

#define KMSG_COMPONENT "ap"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/kernel_stat.h>
#include <linux/moduleparam.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/freezer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <asm/machine.h>
#include <asm/airq.h>
#include <asm/tpi.h>
#include <linux/atomic.h>
#include <asm/isc.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <asm/facility.h>
#include <linux/crypto.h>
#include <linux/mod_devicetable.h>
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/module.h>
#include <asm/uv.h>
#include <asm/chsc.h>
#include <linux/mempool.h>

#include "ap_bus.h"
#include "ap_debug.h"

MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Adjunct Processor Bus driver");
MODULE_LICENSE("GPL");

int ap_domain_index = -1;	/* Adjunct Processor Domain Index */
static DEFINE_SPINLOCK(ap_domain_lock);
module_param_named(domain, ap_domain_index, int, 0440);
MODULE_PARM_DESC(domain, "domain index for ap devices");
EXPORT_SYMBOL(ap_domain_index);

static int ap_thread_flag;
module_param_named(poll_thread, ap_thread_flag, int, 0440);
MODULE_PARM_DESC(poll_thread, "Turn on/off poll thread, default is 0 (off).");

static char *apm_str;
module_param_named(apmask, apm_str, charp, 0440);
MODULE_PARM_DESC(apmask, "AP bus adapter mask.");

static char *aqm_str;
module_param_named(aqmask, aqm_str, charp, 0440);
MODULE_PARM_DESC(aqmask, "AP bus domain mask.");

static int ap_useirq = 1;
module_param_named(useirq, ap_useirq, int, 0440);
MODULE_PARM_DESC(useirq, "Use interrupt if available, default is 1 (on).");

atomic_t ap_max_msg_size = ATOMIC_INIT(AP_DEFAULT_MAX_MSG_SIZE);
EXPORT_SYMBOL(ap_max_msg_size);

static struct device *ap_root_device;

/* Hashtable of all queue devices on the AP bus */
DEFINE_HASHTABLE(ap_queues, 8);
/* lock used for the ap_queues hashtable */
DEFINE_SPINLOCK(ap_queues_lock);

/* Default permissions (ioctl, card and domain masking) */
struct ap_perms ap_perms;
EXPORT_SYMBOL(ap_perms);
DEFINE_MUTEX(ap_perms_mutex);
EXPORT_SYMBOL(ap_perms_mutex);

/* # of bindings complete since init */
static atomic64_t ap_bindings_complete_count = ATOMIC64_INIT(0);

/* completion for APQN bindings complete */
static DECLARE_COMPLETION(ap_apqn_bindings_complete);

static struct ap_config_info qci[2];
static struct ap_config_info *const ap_qci_info = &qci[0];
static struct ap_config_info *const ap_qci_info_old = &qci[1];

/*
 * AP bus related debug feature things.
 */
debug_info_t *ap_dbf_info;

/*
 * There is a need for a do-not-allocate-memory path through the AP bus
 * layer. The pkey layer may be triggered via the in-kernel interface from
 * a protected key crypto algorithm (namely PAES) to convert a secure key
 * into a protected key. This happens in a workqueue context, so sleeping
 * is allowed but memory allocations causing IO operations are not permitted.
 * To accomplish this, an AP message memory pool with pre-allocated space
 * is established. When ap_init_apmsg() with use_mempool set to true is
 * called, instead of kmalloc() the ap message buffer is allocated from
 * the ap_msg_pool. This pool only holds a limited amount of buffers:
 * ap_msg_pool_min_items with the item size AP_DEFAULT_MAX_MSG_SIZE and
 * exactly one of these items (if available) is returned if ap_init_apmsg()
 * with the use_mempool arg set to true is called. When this pool is exhausted
 * and use_mempool is set true, ap_init_apmsg() returns -ENOMEM without
 * any attempt to allocate memory and the caller has to deal with that.
 */
static mempool_t *ap_msg_pool;
static unsigned int ap_msg_pool_min_items = 8;
module_param_named(msgpool_min_items, ap_msg_pool_min_items, uint, 0440);
MODULE_PARM_DESC(msgpool_min_items, "AP message pool minimal items");

/*
 * AP bus rescan related things.
 */
static bool ap_scan_bus(void);
static bool ap_scan_bus_result; /* result of last ap_scan_bus() */
static DEFINE_MUTEX(ap_scan_bus_mutex); /* mutex ap_scan_bus() invocations */
static struct task_struct *ap_scan_bus_task; /* thread holding the scan mutex */
static atomic64_t ap_scan_bus_count; /* counter ap_scan_bus() invocations */
static int ap_scan_bus_time = AP_CONFIG_TIME;
static struct timer_list ap_scan_bus_timer;
static void ap_scan_bus_wq_callback(struct work_struct *);
static DECLARE_WORK(ap_scan_bus_work, ap_scan_bus_wq_callback);

/*
 * Tasklet & timer for AP request polling and interrupts
 */
static void ap_tasklet_fn(unsigned long);
static DECLARE_TASKLET_OLD(ap_tasklet, ap_tasklet_fn);
static DECLARE_WAIT_QUEUE_HEAD(ap_poll_wait);
static struct task_struct *ap_poll_kthread;
static DEFINE_MUTEX(ap_poll_thread_mutex);
static DEFINE_SPINLOCK(ap_poll_timer_lock);
static struct hrtimer ap_poll_timer;
/*
 * In LPAR poll with 4kHz frequency. Poll every 250000 nanoseconds.
 * If z/VM change to 1500000 nanoseconds to adjust to z/VM polling.
 */
static unsigned long poll_high_timeout = 250000UL;

/*
 * Some state machine states only require a low frequency polling.
 * We use 25 Hz frequency for these.
 */
static unsigned long poll_low_timeout = 40000000UL;

/* Maximum domain id, if not given via qci */
static int ap_max_domain_id = 15;
/* Maximum adapter id, if not given via qci */
static int ap_max_adapter_id = 63;

static const struct bus_type ap_bus_type;

/* Adapter interrupt definitions */
static void ap_interrupt_handler(struct airq_struct *airq,
				 struct tpi_info *tpi_info);

static bool ap_irq_flag;

static struct airq_struct ap_airq = {
	.handler = ap_interrupt_handler,
	.isc = AP_ISC,
};

/**
 * ap_airq_ptr() - Get the address of the adapter interrupt indicator
 *
 * Returns the address of the local-summary-indicator of the adapter
 * interrupt handler for AP, or NULL if adapter interrupts are not
 * available.
 */
void *ap_airq_ptr(void)
{
	if (ap_irq_flag)
		return ap_airq.lsi_ptr;
	return NULL;
}

/**
 * ap_interrupts_available(): Test if AP interrupts are available.
 *
 * Returns 1 if AP interrupts are available.
 */
static int ap_interrupts_available(void)
{
	return test_facility(65);
}

/**
 * ap_qci_available(): Test if AP configuration
 * information can be queried via QCI subfunction.
 *
 * Returns 1 if subfunction PQAP(QCI) is available.
 */
static int ap_qci_available(void)
{
	return test_facility(12);
}

/**
 * ap_apft_available(): Test if AP facilities test (APFT)
 * facility is available.
 *
 * Returns 1 if APFT is available.
 */
static int ap_apft_available(void)
{
	return test_facility(15);
}

/*
 * ap_qact_available(): Test if the PQAP(QACT) subfunction is available.
 *
 * Returns 1 if the QACT subfunction is available.
 */
static inline int ap_qact_available(void)
{
	return ap_qci_info->qact;
}

/*
 * ap_sb_available(): Test if the AP secure binding facility is available.
 *
 * Returns 1 if secure binding facility is available.
 */
int ap_sb_available(void)
{
	return ap_qci_info->apsb;
}

/*
 * ap_is_se_guest(): Check for SE guest with AP pass-through support.
 */
bool ap_is_se_guest(void)
{
	return is_prot_virt_guest() && ap_sb_available();
}
EXPORT_SYMBOL(ap_is_se_guest);

/**
 * ap_init_qci_info(): Allocate and query qci config info.
 * Does also update the static variables ap_max_domain_id
 * and ap_max_adapter_id if this info is available.
 */
static void __init ap_init_qci_info(void)
{
	if (!ap_qci_available() ||
	    ap_qci(ap_qci_info)) {
		AP_DBF_INFO("%s QCI not supported\n", __func__);
		return;
	}
	memcpy(ap_qci_info_old, ap_qci_info, sizeof(*ap_qci_info));
	AP_DBF_INFO("%s successful fetched initial qci info\n", __func__);

	if (ap_qci_info->apxa) {
		if (ap_qci_info->na) {
			ap_max_adapter_id = ap_qci_info->na;
			AP_DBF_INFO("%s new ap_max_adapter_id is %d\n",
				    __func__, ap_max_adapter_id);
		}
		if (ap_qci_info->nd) {
			ap_max_domain_id = ap_qci_info->nd;
			AP_DBF_INFO("%s new ap_max_domain_id is %d\n",
				    __func__, ap_max_domain_id);
		}
	}
}

/*
 * ap_test_config(): helper function to extract the nrth bit
 *		     within the unsigned int array field.
 */
static inline int ap_test_config(unsigned int *field, unsigned int nr)
{
	return ap_test_bit((field + (nr >> 5)), (nr & 0x1f));
}

/*
 * ap_test_config_card_id(): Test, whether an AP card ID is configured.
 *
 * Returns 0 if the card is not configured
 *	   1 if the card is configured or
 *	     if the configuration information is not available
 */
static inline int ap_test_config_card_id(unsigned int id)
{
	if (id > ap_max_adapter_id)
		return 0;
	if (ap_qci_info->flags)
		return ap_test_config(ap_qci_info->apm, id);
	return 1;
}

/*
 * ap_test_config_usage_domain(): Test, whether an AP usage domain
 * is configured.
 *
 * Returns 0 if the usage domain is not configured
 *	   1 if the usage domain is configured or
 *	     if the configuration information is not available
 */
int ap_test_config_usage_domain(unsigned int domain)
{
	if (domain > ap_max_domain_id)
		return 0;
	if (ap_qci_info->flags)
		return ap_test_config(ap_qci_info->aqm, domain);
	return 1;
}
EXPORT_SYMBOL(ap_test_config_usage_domain);

/*
 * ap_test_config_ctrl_domain(): Test, whether an AP control domain
 * is configured.
 * @domain AP control domain ID
 *
 * Returns 1 if the control domain is configured
 *	   0 in all other cases
 */
int ap_test_config_ctrl_domain(unsigned int domain)
{
	if (!ap_qci_info || domain > ap_max_domain_id)
		return 0;
	return ap_test_config(ap_qci_info->adm, domain);
}
EXPORT_SYMBOL(ap_test_config_ctrl_domain);

/*
 * ap_queue_info(): Check and get AP queue info.
 * Returns: 1 if APQN exists and info is filled,
 *	    0 if APQN seems to exist but there is no info
 *	      available (eg. caused by an asynch pending error)
 *	   -1 invalid APQN, TAPQ error or AP queue status which
 *	      indicates there is no APQN.
 */
static int ap_queue_info(ap_qid_t qid, struct ap_tapq_hwinfo *hwinfo,
			 bool *decfg, bool *cstop)
{
	struct ap_queue_status status;

	hwinfo->value = 0;

	/* make sure we don't run into a specifiation exception */
	if (AP_QID_CARD(qid) > ap_max_adapter_id ||
	    AP_QID_QUEUE(qid) > ap_max_domain_id)
		return -1;

	/* call TAPQ on this APQN */
	status = ap_test_queue(qid, ap_apft_available(), hwinfo);

	switch (status.response_code) {
	case AP_RESPONSE_NORMAL:
	case AP_RESPONSE_RESET_IN_PROGRESS:
	case AP_RESPONSE_DECONFIGURED:
	case AP_RESPONSE_CHECKSTOPPED:
	case AP_RESPONSE_BUSY:
		/* For all these RCs the tapq info should be available */
		break;
	default:
		/* On a pending async error the info should be available */
		if (!status.async)
			return -1;
		break;
	}

	/* There should be at least one of the mode bits set */
	if (WARN_ON_ONCE(!hwinfo->value))
		return 0;

	*decfg = status.response_code == AP_RESPONSE_DECONFIGURED;
	*cstop = status.response_code == AP_RESPONSE_CHECKSTOPPED;

	return 1;
}

void ap_wait(enum ap_sm_wait wait)
{
	ktime_t hr_time;

	switch (wait) {
	case AP_SM_WAIT_AGAIN:
	case AP_SM_WAIT_INTERRUPT:
		if (ap_irq_flag)
			break;
		if (ap_poll_kthread) {
			wake_up(&ap_poll_wait);
			break;
		}
		fallthrough;
	case AP_SM_WAIT_LOW_TIMEOUT:
	case AP_SM_WAIT_HIGH_TIMEOUT:
		spin_lock_bh(&ap_poll_timer_lock);
		if (!hrtimer_is_queued(&ap_poll_timer)) {
			hr_time =
				wait == AP_SM_WAIT_LOW_TIMEOUT ?
				poll_low_timeout : poll_high_timeout;
			hrtimer_forward_now(&ap_poll_timer, hr_time);
			hrtimer_restart(&ap_poll_timer);
		}
		spin_unlock_bh(&ap_poll_timer_lock);
		break;
	case AP_SM_WAIT_NONE:
	default:
		break;
	}
}

/**
 * ap_request_timeout(): Handling of request timeouts
 * @t: timer making this callback
 *
 * Handles request timeouts.
 */
void ap_request_timeout(struct timer_list *t)
{
	struct ap_queue *aq = timer_container_of(aq, t, timeout);

	spin_lock_bh(&aq->lock);
	ap_wait(ap_sm_event(aq, AP_SM_EVENT_TIMEOUT));
	spin_unlock_bh(&aq->lock);
}

/**
 * ap_poll_timeout(): AP receive polling for finished AP requests.
 * @unused: Unused pointer.
 *
 * Schedules the AP tasklet using a high resolution timer.
 */
static enum hrtimer_restart ap_poll_timeout(struct hrtimer *unused)
{
	tasklet_schedule(&ap_tasklet);
	return HRTIMER_NORESTART;
}

/**
 * ap_interrupt_handler() - Schedule ap_tasklet on interrupt
 * @airq: pointer to adapter interrupt descriptor
 * @tpi_info: ignored
 */
static void ap_interrupt_handler(struct airq_struct *airq,
				 struct tpi_info *tpi_info)
{
	inc_irq_stat(IRQIO_APB);
	tasklet_schedule(&ap_tasklet);
}

/**
 * ap_tasklet_fn(): Tasklet to poll all AP devices.
 * @dummy: Unused variable
 *
 * Poll all AP devices on the bus.
 */
static void ap_tasklet_fn(unsigned long dummy)
{
	int bkt;
	struct ap_queue *aq;
	enum ap_sm_wait wait = AP_SM_WAIT_NONE;

	/* Reset the indicator if interrupts are used. Thus new interrupts can
	 * be received. Doing it in the beginning of the tasklet is therefore
	 * important that no requests on any AP get lost.
	 */
	if (ap_irq_flag)
		WRITE_ONCE(*ap_airq.lsi_ptr, 0);

	spin_lock_bh(&ap_queues_lock);
	hash_for_each(ap_queues, bkt, aq, hnode) {
		spin_lock_bh(&aq->lock);
		wait = min(wait, ap_sm_event_loop(aq, AP_SM_EVENT_POLL));
		spin_unlock_bh(&aq->lock);
	}
	spin_unlock_bh(&ap_queues_lock);

	ap_wait(wait);
}

static int ap_pending_requests(void)
{
	int bkt;
	struct ap_queue *aq;

	spin_lock_bh(&ap_queues_lock);
	hash_for_each(ap_queues, bkt, aq, hnode) {
		if (aq->queue_count == 0)
			continue;
		spin_unlock_bh(&ap_queues_lock);
		return 1;
	}
	spin_unlock_bh(&ap_queues_lock);
	return 0;
}

/**
 * ap_poll_thread(): Thread that polls for finished requests.
 * @data: Unused pointer
 *
 * AP bus poll thread. The purpose of this thread is to poll for
 * finished requests in a loop if there is a "free" cpu - that is
 * a cpu that doesn't have anything better to do. The polling stops
 * as soon as there is another task or if all messages have been
 * delivered.
 */
static int ap_poll_thread(void *data)
{
	DECLARE_WAITQUEUE(wait, current);

	set_user_nice(current, MAX_NICE);
	set_freezable();
	while (!kthread_should_stop()) {
		add_wait_queue(&ap_poll_wait, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		if (!ap_pending_requests()) {
			schedule();
			try_to_freeze();
		}
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&ap_poll_wait, &wait);
		if (need_resched()) {
			schedule();
			try_to_freeze();
			continue;
		}
		ap_tasklet_fn(0);
	}

	return 0;
}

static int ap_poll_thread_start(void)
{
	int rc;

	if (ap_irq_flag || ap_poll_kthread)
		return 0;
	mutex_lock(&ap_poll_thread_mutex);
	ap_poll_kthread = kthread_run(ap_poll_thread, NULL, "appoll");
	rc = PTR_ERR_OR_ZERO(ap_poll_kthread);
	if (rc)
		ap_poll_kthread = NULL;
	mutex_unlock(&ap_poll_thread_mutex);
	return rc;
}

static void ap_poll_thread_stop(void)
{
	if (!ap_poll_kthread)
		return;
	mutex_lock(&ap_poll_thread_mutex);
	kthread_stop(ap_poll_kthread);
	ap_poll_kthread = NULL;
	mutex_unlock(&ap_poll_thread_mutex);
}

#define is_card_dev(x) ((x)->parent == ap_root_device)
#define is_queue_dev(x) ((x)->parent != ap_root_device)

/*
 * ap_init_apmsg() - Initialize ap_message.
 */
int ap_init_apmsg(struct ap_message *ap_msg, u32 flags)
{
	unsigned int maxmsgsize;

	memset(ap_msg, 0, sizeof(*ap_msg));
	ap_msg->flags = flags;

	if (flags & AP_MSG_FLAG_MEMPOOL) {
		ap_msg->msg = mempool_alloc_preallocated(ap_msg_pool);
		if (!ap_msg->msg)
			return -ENOMEM;
		ap_msg->bufsize = AP_DEFAULT_MAX_MSG_SIZE;
		return 0;
	}

	maxmsgsize = atomic_read(&ap_max_msg_size);
	ap_msg->msg = kmalloc(maxmsgsize, GFP_KERNEL);
	if (!ap_msg->msg)
		return -ENOMEM;
	ap_msg->bufsize = maxmsgsize;

	return 0;
}
EXPORT_SYMBOL(ap_init_apmsg);

/*
 * ap_release_apmsg() - Release ap_message.
 */
void ap_release_apmsg(struct ap_message *ap_msg)
{
	if (ap_msg->flags & AP_MSG_FLAG_MEMPOOL) {
		memzero_explicit(ap_msg->msg, ap_msg->bufsize);
		mempool_free(ap_msg->msg, ap_msg_pool);
	} else {
		kfree_sensitive(ap_msg->msg);
	}
}
EXPORT_SYMBOL(ap_release_apmsg);

/**
 * ap_bus_match()
 * @dev: Pointer to device
 * @drv: Pointer to device_driver
 *
 * AP bus driver registration/unregistration.
 */
static int ap_bus_match(struct device *dev, const struct device_driver *drv)
{
	const struct ap_driver *ap_drv = to_ap_drv(drv);
	struct ap_device_id *id;

	/*
	 * Compare device type of the device with the list of
	 * supported types of the device_driver.
	 */
	for (id = ap_drv->ids; id->match_flags; id++) {
		if (is_card_dev(dev) &&
		    id->match_flags & AP_DEVICE_ID_MATCH_CARD_TYPE &&
		    id->dev_type == to_ap_dev(dev)->device_type)
			return 1;
		if (is_queue_dev(dev) &&
		    id->match_flags & AP_DEVICE_ID_MATCH_QUEUE_TYPE &&
		    id->dev_type == to_ap_dev(dev)->device_type)
			return 1;
	}
	return 0;
}

/**
 * ap_uevent(): Uevent function for AP devices.
 * @dev: Pointer to device
 * @env: Pointer to kobj_uevent_env
 *
 * It sets up a single environment variable DEV_TYPE which contains the
 * hardware device type.
 */
static int ap_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	int rc = 0;
	const struct ap_device *ap_dev = to_ap_dev(dev);

	/* Uevents from ap bus core don't need extensions to the env */
	if (dev == ap_root_device)
		return 0;

	if (is_card_dev(dev)) {
		struct ap_card *ac = to_ap_card(&ap_dev->device);

		/* Set up DEV_TYPE environment variable. */
		rc = add_uevent_var(env, "DEV_TYPE=%04X", ap_dev->device_type);
		if (rc)
			return rc;
		/* Add MODALIAS= */
		rc = add_uevent_var(env, "MODALIAS=ap:t%02X", ap_dev->device_type);
		if (rc)
			return rc;

		/* Add MODE=<accel|cca|ep11> */
		if (ac->hwinfo.accel)
			rc = add_uevent_var(env, "MODE=accel");
		else if (ac->hwinfo.cca)
			rc = add_uevent_var(env, "MODE=cca");
		else if (ac->hwinfo.ep11)
			rc = add_uevent_var(env, "MODE=ep11");
		if (rc)
			return rc;
	} else {
		struct ap_queue *aq = to_ap_queue(&ap_dev->device);

		/* Add MODE=<accel|cca|ep11> */
		if (aq->card->hwinfo.accel)
			rc = add_uevent_var(env, "MODE=accel");
		else if (aq->card->hwinfo.cca)
			rc = add_uevent_var(env, "MODE=cca");
		else if (aq->card->hwinfo.ep11)
			rc = add_uevent_var(env, "MODE=ep11");
		if (rc)
			return rc;
	}

	return 0;
}

static void ap_send_init_scan_done_uevent(void)
{
	char *envp[] = { "INITSCAN=done", NULL };

	kobject_uevent_env(&ap_root_device->kobj, KOBJ_CHANGE, envp);
}

static void ap_send_bindings_complete_uevent(void)
{
	char buf[32];
	char *envp[] = { "BINDINGS=complete", buf, NULL };

	snprintf(buf, sizeof(buf), "COMPLETECOUNT=%llu",
		 atomic64_inc_return(&ap_bindings_complete_count));
	kobject_uevent_env(&ap_root_device->kobj, KOBJ_CHANGE, envp);
}

void ap_send_config_uevent(struct ap_device *ap_dev, bool cfg)
{
	char buf[16];
	char *envp[] = { buf, NULL };

	snprintf(buf, sizeof(buf), "CONFIG=%d", cfg ? 1 : 0);

	kobject_uevent_env(&ap_dev->device.kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(ap_send_config_uevent);

void ap_send_online_uevent(struct ap_device *ap_dev, int online)
{
	char buf[16];
	char *envp[] = { buf, NULL };

	snprintf(buf, sizeof(buf), "ONLINE=%d", online ? 1 : 0);

	kobject_uevent_env(&ap_dev->device.kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(ap_send_online_uevent);

static void ap_send_mask_changed_uevent(unsigned long *newapm,
					unsigned long *newaqm)
{
	char buf[100];
	char *envp[] = { buf, NULL };

	if (newapm)
		snprintf(buf, sizeof(buf),
			 "APMASK=0x%016lx%016lx%016lx%016lx\n",
			 newapm[0], newapm[1], newapm[2], newapm[3]);
	else
		snprintf(buf, sizeof(buf),
			 "AQMASK=0x%016lx%016lx%016lx%016lx\n",
			 newaqm[0], newaqm[1], newaqm[2], newaqm[3]);

	kobject_uevent_env(&ap_root_device->kobj, KOBJ_CHANGE, envp);
}

/*
 * calc # of bound APQNs
 */

struct __ap_calc_ctrs {
	unsigned int apqns;
	unsigned int bound;
};

static int __ap_calc_helper(struct device *dev, void *arg)
{
	struct __ap_calc_ctrs *pctrs = (struct __ap_calc_ctrs *)arg;

	if (is_queue_dev(dev)) {
		pctrs->apqns++;
		if (dev->driver)
			pctrs->bound++;
	}

	return 0;
}

static void ap_calc_bound_apqns(unsigned int *apqns, unsigned int *bound)
{
	struct __ap_calc_ctrs ctrs;

	memset(&ctrs, 0, sizeof(ctrs));
	bus_for_each_dev(&ap_bus_type, NULL, (void *)&ctrs, __ap_calc_helper);

	*apqns = ctrs.apqns;
	*bound = ctrs.bound;
}

/*
 * After ap bus scan do check if all existing APQNs are
 * bound to device drivers.
 */
static void ap_check_bindings_complete(void)
{
	unsigned int apqns, bound;

	if (atomic64_read(&ap_scan_bus_count) >= 1) {
		ap_calc_bound_apqns(&apqns, &bound);
		if (bound == apqns) {
			if (!completion_done(&ap_apqn_bindings_complete)) {
				complete_all(&ap_apqn_bindings_complete);
				ap_send_bindings_complete_uevent();
				pr_debug("all apqn bindings complete\n");
			}
		}
	}
}

/*
 * Interface to wait for the AP bus to have done one initial ap bus
 * scan and all detected APQNs have been bound to device drivers.
 * If these both conditions are not fulfilled, this function blocks
 * on a condition with wait_for_completion_interruptible_timeout().
 * If these both conditions are fulfilled (before the timeout hits)
 * the return value is 0. If the timeout (in jiffies) hits instead
 * -ETIME is returned. On failures negative return values are
 * returned to the caller.
 */
int ap_wait_apqn_bindings_complete(unsigned long timeout)
{
	int rc = 0;
	long l;

	if (completion_done(&ap_apqn_bindings_complete))
		return 0;

	if (timeout)
		l = wait_for_completion_interruptible_timeout(
			&ap_apqn_bindings_complete, timeout);
	else
		l = wait_for_completion_interruptible(
			&ap_apqn_bindings_complete);
	if (l < 0)
		rc = l == -ERESTARTSYS ? -EINTR : l;
	else if (l == 0 && timeout)
		rc = -ETIME;

	pr_debug("rc=%d\n", rc);
	return rc;
}
EXPORT_SYMBOL(ap_wait_apqn_bindings_complete);

static int __ap_queue_devices_with_id_unregister(struct device *dev, void *data)
{
	if (is_queue_dev(dev) &&
	    AP_QID_CARD(to_ap_queue(dev)->qid) == (int)(long)data)
		device_unregister(dev);
	return 0;
}

static int __ap_revise_reserved(struct device *dev, void *dummy)
{
	int rc, card, queue, devres, drvres;

	if (is_queue_dev(dev)) {
		card = AP_QID_CARD(to_ap_queue(dev)->qid);
		queue = AP_QID_QUEUE(to_ap_queue(dev)->qid);
		mutex_lock(&ap_perms_mutex);
		devres = test_bit_inv(card, ap_perms.apm) &&
			test_bit_inv(queue, ap_perms.aqm);
		mutex_unlock(&ap_perms_mutex);
		drvres = to_ap_drv(dev->driver)->flags
			& AP_DRIVER_FLAG_DEFAULT;
		if (!!devres != !!drvres) {
			pr_debug("reprobing queue=%02x.%04x\n", card, queue);
			rc = device_reprobe(dev);
			if (rc)
				AP_DBF_WARN("%s reprobing queue=%02x.%04x failed\n",
					    __func__, card, queue);
		}
	}

	return 0;
}

static void ap_bus_revise_bindings(void)
{
	bus_for_each_dev(&ap_bus_type, NULL, NULL, __ap_revise_reserved);
}

/**
 * ap_owned_by_def_drv: indicates whether an AP adapter is reserved for the
 *			default host driver or not.
 * @card: the APID of the adapter card to check
 * @queue: the APQI of the queue to check
 *
 * Note: the ap_perms_mutex must be locked by the caller of this function.
 *
 * Return: an int specifying whether the AP adapter is reserved for the host (1)
 *	   or not (0).
 */
int ap_owned_by_def_drv(int card, int queue)
{
	int rc = 0;

	if (card < 0 || card >= AP_DEVICES || queue < 0 || queue >= AP_DOMAINS)
		return -EINVAL;

	if (test_bit_inv(card, ap_perms.apm) &&
	    test_bit_inv(queue, ap_perms.aqm))
		rc = 1;

	return rc;
}
EXPORT_SYMBOL(ap_owned_by_def_drv);

/**
 * ap_apqn_in_matrix_owned_by_def_drv: indicates whether every APQN contained in
 *				       a set is reserved for the host drivers
 *				       or not.
 * @apm: a bitmap specifying a set of APIDs comprising the APQNs to check
 * @aqm: a bitmap specifying a set of APQIs comprising the APQNs to check
 *
 * Note: the ap_perms_mutex must be locked by the caller of this function.
 *
 * Return: an int specifying whether each APQN is reserved for the host (1) or
 *	   not (0)
 */
int ap_apqn_in_matrix_owned_by_def_drv(unsigned long *apm,
				       unsigned long *aqm)
{
	int card, queue, rc = 0;

	for (card = 0; !rc && card < AP_DEVICES; card++)
		if (test_bit_inv(card, apm) &&
		    test_bit_inv(card, ap_perms.apm))
			for (queue = 0; !rc && queue < AP_DOMAINS; queue++)
				if (test_bit_inv(queue, aqm) &&
				    test_bit_inv(queue, ap_perms.aqm))
					rc = 1;

	return rc;
}
EXPORT_SYMBOL(ap_apqn_in_matrix_owned_by_def_drv);

static int ap_device_probe(struct device *dev)
{
	struct ap_device *ap_dev = to_ap_dev(dev);
	struct ap_driver *ap_drv = to_ap_drv(dev->driver);
	int card, queue, devres, drvres, rc = -ENODEV;

	if (!get_device(dev))
		return rc;

	if (is_queue_dev(dev)) {
		/*
		 * If the apqn is marked as reserved/used by ap bus and
		 * default drivers, only probe with drivers with the default
		 * flag set. If it is not marked, only probe with drivers
		 * with the default flag not set.
		 */
		card = AP_QID_CARD(to_ap_queue(dev)->qid);
		queue = AP_QID_QUEUE(to_ap_queue(dev)->qid);
		mutex_lock(&ap_perms_mutex);
		devres = test_bit_inv(card, ap_perms.apm) &&
			test_bit_inv(queue, ap_perms.aqm);
		mutex_unlock(&ap_perms_mutex);
		drvres = ap_drv->flags & AP_DRIVER_FLAG_DEFAULT;
		if (!!devres != !!drvres)
			goto out;
	}

	/*
	 * Rearm the bindings complete completion to trigger
	 * bindings complete when all devices are bound again
	 */
	reinit_completion(&ap_apqn_bindings_complete);

	/* Add queue/card to list of active queues/cards */
	spin_lock_bh(&ap_queues_lock);
	if (is_queue_dev(dev))
		hash_add(ap_queues, &to_ap_queue(dev)->hnode,
			 to_ap_queue(dev)->qid);
	spin_unlock_bh(&ap_queues_lock);

	rc = ap_drv->probe ? ap_drv->probe(ap_dev) : -ENODEV;

	if (rc) {
		spin_lock_bh(&ap_queues_lock);
		if (is_queue_dev(dev))
			hash_del(&to_ap_queue(dev)->hnode);
		spin_unlock_bh(&ap_queues_lock);
	}

out:
	if (rc)
		put_device(dev);
	return rc;
}

static void ap_device_remove(struct device *dev)
{
	struct ap_device *ap_dev = to_ap_dev(dev);
	struct ap_driver *ap_drv = to_ap_drv(dev->driver);

	/* prepare ap queue device removal */
	if (is_queue_dev(dev))
		ap_queue_prepare_remove(to_ap_queue(dev));

	/* driver's chance to clean up gracefully */
	if (ap_drv->remove)
		ap_drv->remove(ap_dev);

	/* now do the ap queue device remove */
	if (is_queue_dev(dev))
		ap_queue_remove(to_ap_queue(dev));

	/* Remove queue/card from list of active queues/cards */
	spin_lock_bh(&ap_queues_lock);
	if (is_queue_dev(dev))
		hash_del(&to_ap_queue(dev)->hnode);
	spin_unlock_bh(&ap_queues_lock);

	put_device(dev);
}

struct ap_queue *ap_get_qdev(ap_qid_t qid)
{
	int bkt;
	struct ap_queue *aq;

	spin_lock_bh(&ap_queues_lock);
	hash_for_each(ap_queues, bkt, aq, hnode) {
		if (aq->qid == qid) {
			get_device(&aq->ap_dev.device);
			spin_unlock_bh(&ap_queues_lock);
			return aq;
		}
	}
	spin_unlock_bh(&ap_queues_lock);

	return NULL;
}
EXPORT_SYMBOL(ap_get_qdev);

int ap_driver_register(struct ap_driver *ap_drv, struct module *owner,
		       char *name)
{
	struct device_driver *drv = &ap_drv->driver;
	int rc;

	drv->bus = &ap_bus_type;
	drv->owner = owner;
	drv->name = name;
	rc = driver_register(drv);

	ap_check_bindings_complete();

	return rc;
}
EXPORT_SYMBOL(ap_driver_register);

void ap_driver_unregister(struct ap_driver *ap_drv)
{
	driver_unregister(&ap_drv->driver);
}
EXPORT_SYMBOL(ap_driver_unregister);

/*
 * Enforce a synchronous AP bus rescan.
 * Returns true if the bus scan finds a change in the AP configuration
 * and AP devices have been added or deleted when this function returns.
 */
bool ap_bus_force_rescan(void)
{
	unsigned long scan_counter = atomic64_read(&ap_scan_bus_count);
	bool rc = false;

	pr_debug("> scan counter=%lu\n", scan_counter);

	/* Only trigger AP bus scans after the initial scan is done */
	if (scan_counter <= 0)
		goto out;

	/*
	 * There is one unlikely but nevertheless valid scenario where the
	 * thread holding the mutex may try to send some crypto load but
	 * all cards are offline so a rescan is triggered which causes
	 * a recursive call of ap_bus_force_rescan(). A simple return if
	 * the mutex is already locked by this thread solves this.
	 */
	if (mutex_is_locked(&ap_scan_bus_mutex)) {
		if (ap_scan_bus_task == current)
			goto out;
	}

	/* Try to acquire the AP scan bus mutex */
	if (mutex_trylock(&ap_scan_bus_mutex)) {
		/* mutex acquired, run the AP bus scan */
		ap_scan_bus_task = current;
		ap_scan_bus_result = ap_scan_bus();
		rc = ap_scan_bus_result;
		ap_scan_bus_task = NULL;
		mutex_unlock(&ap_scan_bus_mutex);
		goto out;
	}

	/*
	 * Mutex acquire failed. So there is currently another task
	 * already running the AP bus scan. Then let's simple wait
	 * for the lock which means the other task has finished and
	 * stored the result in ap_scan_bus_result.
	 */
	if (mutex_lock_interruptible(&ap_scan_bus_mutex)) {
		/* some error occurred, ignore and go out */
		goto out;
	}
	rc = ap_scan_bus_result;
	mutex_unlock(&ap_scan_bus_mutex);

out:
	pr_debug("rc=%d\n", rc);
	return rc;
}
EXPORT_SYMBOL(ap_bus_force_rescan);

/*
 * A config change has happened, force an ap bus rescan.
 */
static int ap_bus_cfg_chg(struct notifier_block *nb,
			  unsigned long action, void *data)
{
	if (action != CHSC_NOTIFY_AP_CFG)
		return NOTIFY_DONE;

	pr_debug("config change, forcing bus rescan\n");

	ap_bus_force_rescan();

	return NOTIFY_OK;
}

static struct notifier_block ap_bus_nb = {
	.notifier_call = ap_bus_cfg_chg,
};

int ap_hex2bitmap(const char *str, unsigned long *bitmap, int bits)
{
	int i, n, b;

	/* bits needs to be a multiple of 8 */
	if (bits & 0x07)
		return -EINVAL;

	if (str[0] == '0' && str[1] == 'x')
		str++;
	if (*str == 'x')
		str++;

	for (i = 0; isxdigit(*str) && i < bits; str++) {
		b = hex_to_bin(*str);
		for (n = 0; n < 4; n++)
			if (b & (0x08 >> n))
				set_bit_inv(i + n, bitmap);
		i += 4;
	}

	if (*str == '\n')
		str++;
	if (*str)
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL(ap_hex2bitmap);

/*
 * modify_bitmap() - parse bitmask argument and modify an existing
 * bit mask accordingly. A concatenation (done with ',') of these
 * terms is recognized:
 *   +<bitnr>[-<bitnr>] or -<bitnr>[-<bitnr>]
 * <bitnr> may be any valid number (hex, decimal or octal) in the range
 * 0...bits-1; the leading + or - is required. Here are some examples:
 *   +0-15,+32,-128,-0xFF
 *   -0-255,+1-16,+0x128
 *   +1,+2,+3,+4,-5,-7-10
 * Returns the new bitmap after all changes have been applied. Every
 * positive value in the string will set a bit and every negative value
 * in the string will clear a bit. As a bit may be touched more than once,
 * the last 'operation' wins:
 * +0-255,-128 = first bits 0-255 will be set, then bit 128 will be
 * cleared again. All other bits are unmodified.
 */
static int modify_bitmap(const char *str, unsigned long *bitmap, int bits)
{
	unsigned long a, i, z;
	char *np, sign;

	/* bits needs to be a multiple of 8 */
	if (bits & 0x07)
		return -EINVAL;

	while (*str) {
		sign = *str++;
		if (sign != '+' && sign != '-')
			return -EINVAL;
		a = z = simple_strtoul(str, &np, 0);
		if (str == np || a >= bits)
			return -EINVAL;
		str = np;
		if (*str == '-') {
			z = simple_strtoul(++str, &np, 0);
			if (str == np || a > z || z >= bits)
				return -EINVAL;
			str = np;
		}
		for (i = a; i <= z; i++)
			if (sign == '+')
				set_bit_inv(i, bitmap);
			else
				clear_bit_inv(i, bitmap);
		while (*str == ',' || *str == '\n')
			str++;
	}

	return 0;
}

static int ap_parse_bitmap_str(const char *str, unsigned long *bitmap, int bits,
			       unsigned long *newmap)
{
	unsigned long size;
	int rc;

	size = BITS_TO_LONGS(bits) * sizeof(unsigned long);
	if (*str == '+' || *str == '-') {
		memcpy(newmap, bitmap, size);
		rc = modify_bitmap(str, newmap, bits);
	} else {
		memset(newmap, 0, size);
		rc = ap_hex2bitmap(str, newmap, bits);
	}
	return rc;
}

int ap_parse_mask_str(const char *str,
		      unsigned long *bitmap, int bits,
		      struct mutex *lock)
{
	unsigned long *newmap, size;
	int rc;

	/* bits needs to be a multiple of 8 */
	if (bits & 0x07)
		return -EINVAL;

	size = BITS_TO_LONGS(bits) * sizeof(unsigned long);
	newmap = kmalloc(size, GFP_KERNEL);
	if (!newmap)
		return -ENOMEM;
	if (mutex_lock_interruptible(lock)) {
		kfree(newmap);
		return -ERESTARTSYS;
	}
	rc = ap_parse_bitmap_str(str, bitmap, bits, newmap);
	if (rc == 0)
		memcpy(bitmap, newmap, size);
	mutex_unlock(lock);
	kfree(newmap);
	return rc;
}
EXPORT_SYMBOL(ap_parse_mask_str);

/*
 * AP bus attributes.
 */

static ssize_t ap_domain_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_domain_index);
}

static ssize_t ap_domain_store(const struct bus_type *bus,
			       const char *buf, size_t count)
{
	int domain;

	if (sscanf(buf, "%i\n", &domain) != 1 ||
	    domain < 0 || domain > ap_max_domain_id ||
	    !test_bit_inv(domain, ap_perms.aqm))
		return -EINVAL;

	spin_lock_bh(&ap_domain_lock);
	ap_domain_index = domain;
	spin_unlock_bh(&ap_domain_lock);

	AP_DBF_INFO("%s stored new default domain=%d\n",
		    __func__, domain);

	return count;
}

static BUS_ATTR_RW(ap_domain);

static ssize_t ap_control_domain_mask_show(const struct bus_type *bus, char *buf)
{
	if (!ap_qci_info->flags)	/* QCI not supported */
		return sysfs_emit(buf, "not supported\n");

	return sysfs_emit(buf, "0x%08x%08x%08x%08x%08x%08x%08x%08x\n",
			  ap_qci_info->adm[0], ap_qci_info->adm[1],
			  ap_qci_info->adm[2], ap_qci_info->adm[3],
			  ap_qci_info->adm[4], ap_qci_info->adm[5],
			  ap_qci_info->adm[6], ap_qci_info->adm[7]);
}

static BUS_ATTR_RO(ap_control_domain_mask);

static ssize_t ap_usage_domain_mask_show(const struct bus_type *bus, char *buf)
{
	if (!ap_qci_info->flags)	/* QCI not supported */
		return sysfs_emit(buf, "not supported\n");

	return sysfs_emit(buf, "0x%08x%08x%08x%08x%08x%08x%08x%08x\n",
			  ap_qci_info->aqm[0], ap_qci_info->aqm[1],
			  ap_qci_info->aqm[2], ap_qci_info->aqm[3],
			  ap_qci_info->aqm[4], ap_qci_info->aqm[5],
			  ap_qci_info->aqm[6], ap_qci_info->aqm[7]);
}

static BUS_ATTR_RO(ap_usage_domain_mask);

static ssize_t ap_adapter_mask_show(const struct bus_type *bus, char *buf)
{
	if (!ap_qci_info->flags)	/* QCI not supported */
		return sysfs_emit(buf, "not supported\n");

	return sysfs_emit(buf, "0x%08x%08x%08x%08x%08x%08x%08x%08x\n",
			  ap_qci_info->apm[0], ap_qci_info->apm[1],
			  ap_qci_info->apm[2], ap_qci_info->apm[3],
			  ap_qci_info->apm[4], ap_qci_info->apm[5],
			  ap_qci_info->apm[6], ap_qci_info->apm[7]);
}

static BUS_ATTR_RO(ap_adapter_mask);

static ssize_t ap_interrupts_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_irq_flag ? 1 : 0);
}

static BUS_ATTR_RO(ap_interrupts);

static ssize_t config_time_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_scan_bus_time);
}

static ssize_t config_time_store(const struct bus_type *bus,
				 const char *buf, size_t count)
{
	int time;

	if (sscanf(buf, "%d\n", &time) != 1 || time < 5 || time > 120)
		return -EINVAL;
	ap_scan_bus_time = time;
	mod_timer(&ap_scan_bus_timer, jiffies + ap_scan_bus_time * HZ);
	return count;
}

static BUS_ATTR_RW(config_time);

static ssize_t poll_thread_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_poll_kthread ? 1 : 0);
}

static ssize_t poll_thread_store(const struct bus_type *bus,
				 const char *buf, size_t count)
{
	bool value;
	int rc;

	rc = kstrtobool(buf, &value);
	if (rc)
		return rc;

	if (value) {
		rc = ap_poll_thread_start();
		if (rc)
			count = rc;
	} else {
		ap_poll_thread_stop();
	}
	return count;
}

static BUS_ATTR_RW(poll_thread);

static ssize_t poll_timeout_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%lu\n", poll_high_timeout);
}

static ssize_t poll_timeout_store(const struct bus_type *bus, const char *buf,
				  size_t count)
{
	unsigned long value;
	ktime_t hr_time;
	int rc;

	rc = kstrtoul(buf, 0, &value);
	if (rc)
		return rc;

	/* 120 seconds = maximum poll interval */
	if (value > 120000000000UL)
		return -EINVAL;
	poll_high_timeout = value;
	hr_time = poll_high_timeout;

	spin_lock_bh(&ap_poll_timer_lock);
	hrtimer_cancel(&ap_poll_timer);
	hrtimer_set_expires(&ap_poll_timer, hr_time);
	hrtimer_start_expires(&ap_poll_timer, HRTIMER_MODE_ABS);
	spin_unlock_bh(&ap_poll_timer_lock);

	return count;
}

static BUS_ATTR_RW(poll_timeout);

static ssize_t ap_max_domain_id_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_max_domain_id);
}

static BUS_ATTR_RO(ap_max_domain_id);

static ssize_t ap_max_adapter_id_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%d\n", ap_max_adapter_id);
}

static BUS_ATTR_RO(ap_max_adapter_id);

static ssize_t apmask_show(const struct bus_type *bus, char *buf)
{
	int rc;

	if (mutex_lock_interruptible(&ap_perms_mutex))
		return -ERESTARTSYS;
	rc = sysfs_emit(buf, "0x%016lx%016lx%016lx%016lx\n",
			ap_perms.apm[0], ap_perms.apm[1],
			ap_perms.apm[2], ap_perms.apm[3]);
	mutex_unlock(&ap_perms_mutex);

	return rc;
}

static int __verify_card_reservations(struct device_driver *drv, void *data)
{
	int rc = 0;
	struct ap_driver *ap_drv = to_ap_drv(drv);
	unsigned long *newapm = (unsigned long *)data;

	/*
	 * increase the driver's module refcounter to be sure it is not
	 * going away when we invoke the callback function.
	 */
	if (!try_module_get(drv->owner))
		return 0;

	if (ap_drv->in_use) {
		rc = ap_drv->in_use(newapm, ap_perms.aqm);
		if (rc)
			rc = -EBUSY;
	}

	/* release the driver's module */
	module_put(drv->owner);

	return rc;
}

static int apmask_commit(unsigned long *newapm)
{
	int rc;
	unsigned long reserved[BITS_TO_LONGS(AP_DEVICES)];

	/*
	 * Check if any bits in the apmask have been set which will
	 * result in queues being removed from non-default drivers
	 */
	if (bitmap_andnot(reserved, newapm, ap_perms.apm, AP_DEVICES)) {
		rc = bus_for_each_drv(&ap_bus_type, NULL, reserved,
				      __verify_card_reservations);
		if (rc)
			return rc;
	}

	memcpy(ap_perms.apm, newapm, APMASKSIZE);

	return 0;
}

static ssize_t apmask_store(const struct bus_type *bus, const char *buf,
			    size_t count)
{
	int rc, changes = 0;
	DECLARE_BITMAP(newapm, AP_DEVICES);

	if (mutex_lock_interruptible(&ap_perms_mutex))
		return -ERESTARTSYS;

	rc = ap_parse_bitmap_str(buf, ap_perms.apm, AP_DEVICES, newapm);
	if (rc)
		goto done;

	changes = memcmp(ap_perms.apm, newapm, APMASKSIZE);
	if (changes)
		rc = apmask_commit(newapm);

done:
	mutex_unlock(&ap_perms_mutex);
	if (rc)
		return rc;

	if (changes) {
		ap_bus_revise_bindings();
		ap_send_mask_changed_uevent(newapm, NULL);
	}

	return count;
}

static BUS_ATTR_RW(apmask);

static ssize_t aqmask_show(const struct bus_type *bus, char *buf)
{
	int rc;

	if (mutex_lock_interruptible(&ap_perms_mutex))
		return -ERESTARTSYS;
	rc = sysfs_emit(buf, "0x%016lx%016lx%016lx%016lx\n",
			ap_perms.aqm[0], ap_perms.aqm[1],
			ap_perms.aqm[2], ap_perms.aqm[3]);
	mutex_unlock(&ap_perms_mutex);

	return rc;
}

static int __verify_queue_reservations(struct device_driver *drv, void *data)
{
	int rc = 0;
	struct ap_driver *ap_drv = to_ap_drv(drv);
	unsigned long *newaqm = (unsigned long *)data;

	/*
	 * increase the driver's module refcounter to be sure it is not
	 * going away when we invoke the callback function.
	 */
	if (!try_module_get(drv->owner))
		return 0;

	if (ap_drv->in_use) {
		rc = ap_drv->in_use(ap_perms.apm, newaqm);
		if (rc)
			rc = -EBUSY;
	}

	/* release the driver's module */
	module_put(drv->owner);

	return rc;
}

static int aqmask_commit(unsigned long *newaqm)
{
	int rc;
	unsigned long reserved[BITS_TO_LONGS(AP_DOMAINS)];

	/*
	 * Check if any bits in the aqmask have been set which will
	 * result in queues being removed from non-default drivers
	 */
	if (bitmap_andnot(reserved, newaqm, ap_perms.aqm, AP_DOMAINS)) {
		rc = bus_for_each_drv(&ap_bus_type, NULL, reserved,
				      __verify_queue_reservations);
		if (rc)
			return rc;
	}

	memcpy(ap_perms.aqm, newaqm, AQMASKSIZE);

	return 0;
}

static ssize_t aqmask_store(const struct bus_type *bus, const char *buf,
			    size_t count)
{
	int rc, changes = 0;
	DECLARE_BITMAP(newaqm, AP_DOMAINS);

	if (mutex_lock_interruptible(&ap_perms_mutex))
		return -ERESTARTSYS;

	rc = ap_parse_bitmap_str(buf, ap_perms.aqm, AP_DOMAINS, newaqm);
	if (rc)
		goto done;

	changes = memcmp(ap_perms.aqm, newaqm, APMASKSIZE);
	if (changes)
		rc = aqmask_commit(newaqm);

done:
	mutex_unlock(&ap_perms_mutex);
	if (rc)
		return rc;

	if (changes) {
		ap_bus_revise_bindings();
		ap_send_mask_changed_uevent(NULL, newaqm);
	}

	return count;
}

static BUS_ATTR_RW(aqmask);

static ssize_t scans_show(const struct bus_type *bus, char *buf)
{
	return sysfs_emit(buf, "%llu\n", atomic64_read(&ap_scan_bus_count));
}

static ssize_t scans_store(const struct bus_type *bus, const char *buf,
			   size_t count)
{
	AP_DBF_INFO("%s force AP bus rescan\n", __func__);

	ap_bus_force_rescan();

	return count;
}

static BUS_ATTR_RW(scans);

static ssize_t bindings_show(const struct bus_type *bus, char *buf)
{
	int rc;
	unsigned int apqns, n;

	ap_calc_bound_apqns(&apqns, &n);
	if (atomic64_read(&ap_scan_bus_count) >= 1 && n == apqns)
		rc = sysfs_emit(buf, "%u/%u (complete)\n", n, apqns);
	else
		rc = sysfs_emit(buf, "%u/%u\n", n, apqns);

	return rc;
}

static BUS_ATTR_RO(bindings);

static ssize_t features_show(const struct bus_type *bus, char *buf)
{
	int n = 0;

	if (!ap_qci_info->flags)	/* QCI not supported */
		return sysfs_emit(buf, "-\n");

	if (ap_qci_info->apsc)
		n += sysfs_emit_at(buf, n, "APSC ");
	if (ap_qci_info->apxa)
		n += sysfs_emit_at(buf, n, "APXA ");
	if (ap_qci_info->qact)
		n += sysfs_emit_at(buf, n, "QACT ");
	if (ap_qci_info->rc8a)
		n += sysfs_emit_at(buf, n, "RC8A ");
	if (ap_qci_info->apsb)
		n += sysfs_emit_at(buf, n, "APSB ");

	sysfs_emit_at(buf, n == 0 ? 0 : n - 1, "\n");

	return n;
}

static BUS_ATTR_RO(features);

static struct attribute *ap_bus_attrs[] = {
	&bus_attr_ap_domain.attr,
	&bus_attr_ap_control_domain_mask.attr,
	&bus_attr_ap_usage_domain_mask.attr,
	&bus_attr_ap_adapter_mask.attr,
	&bus_attr_config_time.attr,
	&bus_attr_poll_thread.attr,
	&bus_attr_ap_interrupts.attr,
	&bus_attr_poll_timeout.attr,
	&bus_attr_ap_max_domain_id.attr,
	&bus_attr_ap_max_adapter_id.attr,
	&bus_attr_apmask.attr,
	&bus_attr_aqmask.attr,
	&bus_attr_scans.attr,
	&bus_attr_bindings.attr,
	&bus_attr_features.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ap_bus);

static const struct bus_type ap_bus_type = {
	.name = "ap",
	.bus_groups = ap_bus_groups,
	.match = &ap_bus_match,
	.uevent = &ap_uevent,
	.probe = ap_device_probe,
	.remove = ap_device_remove,
};

/**
 * ap_select_domain(): Select an AP domain if possible and we haven't
 * already done so before.
 */
static void ap_select_domain(void)
{
	struct ap_queue_status status;
	int card, dom;

	/*
	 * Choose the default domain. Either the one specified with
	 * the "domain=" parameter or the first domain with at least
	 * one valid APQN.
	 */
	spin_lock_bh(&ap_domain_lock);
	if (ap_domain_index >= 0) {
		/* Domain has already been selected. */
		goto out;
	}
	for (dom = 0; dom <= ap_max_domain_id; dom++) {
		if (!ap_test_config_usage_domain(dom) ||
		    !test_bit_inv(dom, ap_perms.aqm))
			continue;
		for (card = 0; card <= ap_max_adapter_id; card++) {
			if (!ap_test_config_card_id(card) ||
			    !test_bit_inv(card, ap_perms.apm))
				continue;
			status = ap_test_queue(AP_MKQID(card, dom),
					       ap_apft_available(),
					       NULL);
			if (status.response_code == AP_RESPONSE_NORMAL)
				break;
		}
		if (card <= ap_max_adapter_id)
			break;
	}
	if (dom <= ap_max_domain_id) {
		ap_domain_index = dom;
		AP_DBF_INFO("%s new default domain is %d\n",
			    __func__, ap_domain_index);
	}
out:
	spin_unlock_bh(&ap_domain_lock);
}

/*
 * This function checks the type and returns either 0 for not
 * supported or the highest compatible type value (which may
 * include the input type value).
 */
static int ap_get_compatible_type(ap_qid_t qid, int rawtype, unsigned int func)
{
	int comp_type = 0;

	/* < CEX4 is not supported */
	if (rawtype < AP_DEVICE_TYPE_CEX4) {
		AP_DBF_WARN("%s queue=%02x.%04x unsupported type %d\n",
			    __func__, AP_QID_CARD(qid),
			    AP_QID_QUEUE(qid), rawtype);
		return 0;
	}
	/* up to CEX8 known and fully supported */
	if (rawtype <= AP_DEVICE_TYPE_CEX8)
		return rawtype;
	/*
	 * unknown new type > CEX8, check for compatibility
	 * to the highest known and supported type which is
	 * currently CEX8 with the help of the QACT function.
	 */
	if (ap_qact_available()) {
		struct ap_queue_status status;
		union ap_qact_ap_info apinfo = {0};

		apinfo.mode = (func >> 26) & 0x07;
		apinfo.cat = AP_DEVICE_TYPE_CEX8;
		status = ap_qact(qid, 0, &apinfo);
		if (status.response_code == AP_RESPONSE_NORMAL &&
		    apinfo.cat >= AP_DEVICE_TYPE_CEX4 &&
		    apinfo.cat <= AP_DEVICE_TYPE_CEX8)
			comp_type = apinfo.cat;
	}
	if (!comp_type)
		AP_DBF_WARN("%s queue=%02x.%04x unable to map type %d\n",
			    __func__, AP_QID_CARD(qid),
			    AP_QID_QUEUE(qid), rawtype);
	else if (comp_type != rawtype)
		AP_DBF_INFO("%s queue=%02x.%04x map type %d to %d\n",
			    __func__, AP_QID_CARD(qid), AP_QID_QUEUE(qid),
			    rawtype, comp_type);
	return comp_type;
}

/*
 * Helper function to be used with bus_find_dev
 * matches for the card device with the given id
 */
static int __match_card_device_with_id(struct device *dev, const void *data)
{
	return is_card_dev(dev) && to_ap_card(dev)->id == (int)(long)(void *)data;
}

/*
 * Helper function to be used with bus_find_dev
 * matches for the queue device with a given qid
 */
static int __match_queue_device_with_qid(struct device *dev, const void *data)
{
	return is_queue_dev(dev) && to_ap_queue(dev)->qid == (int)(long)data;
}

/*
 * Helper function to be used with bus_find_dev
 * matches any queue device with given queue id
 */
static int __match_queue_device_with_queue_id(struct device *dev, const void *data)
{
	return is_queue_dev(dev) &&
		AP_QID_QUEUE(to_ap_queue(dev)->qid) == (int)(long)data;
}

/* Helper function for notify_config_changed */
static int __drv_notify_config_changed(struct device_driver *drv, void *data)
{
	struct ap_driver *ap_drv = to_ap_drv(drv);

	if (try_module_get(drv->owner)) {
		if (ap_drv->on_config_changed)
			ap_drv->on_config_changed(ap_qci_info, ap_qci_info_old);
		module_put(drv->owner);
	}

	return 0;
}

/* Notify all drivers about an qci config change */
static inline void notify_config_changed(void)
{
	bus_for_each_drv(&ap_bus_type, NULL, NULL,
			 __drv_notify_config_changed);
}

/* Helper function for notify_scan_complete */
static int __drv_notify_scan_complete(struct device_driver *drv, void *data)
{
	struct ap_driver *ap_drv = to_ap_drv(drv);

	if (try_module_get(drv->owner)) {
		if (ap_drv->on_scan_complete)
			ap_drv->on_scan_complete(ap_qci_info,
						 ap_qci_info_old);
		module_put(drv->owner);
	}

	return 0;
}

/* Notify all drivers about bus scan complete */
static inline void notify_scan_complete(void)
{
	bus_for_each_drv(&ap_bus_type, NULL, NULL,
			 __drv_notify_scan_complete);
}

/*
 * Helper function for ap_scan_bus().
 * Remove card device and associated queue devices.
 */
static inline void ap_scan_rm_card_dev_and_queue_devs(struct ap_card *ac)
{
	bus_for_each_dev(&ap_bus_type, NULL,
			 (void *)(long)ac->id,
			 __ap_queue_devices_with_id_unregister);
	device_unregister(&ac->ap_dev.device);
}

/*
 * Helper function for ap_scan_bus().
 * Does the scan bus job for all the domains within
 * a valid adapter given by an ap_card ptr.
 */
static inline void ap_scan_domains(struct ap_card *ac)
{
	struct ap_tapq_hwinfo hwinfo;
	bool decfg, chkstop;
	struct ap_queue *aq;
	struct device *dev;
	ap_qid_t qid;
	int rc, dom;

	/*
	 * Go through the configuration for the domains and compare them
	 * to the existing queue devices. Also take care of the config
	 * and error state for the queue devices.
	 */

	for (dom = 0; dom <= ap_max_domain_id; dom++) {
		qid = AP_MKQID(ac->id, dom);
		dev = bus_find_device(&ap_bus_type, NULL,
				      (void *)(long)qid,
				      __match_queue_device_with_qid);
		aq = dev ? to_ap_queue(dev) : NULL;
		if (!ap_test_config_usage_domain(dom)) {
			if (dev) {
				AP_DBF_INFO("%s(%d,%d) not in config anymore, rm queue dev\n",
					    __func__, ac->id, dom);
				device_unregister(dev);
			}
			goto put_dev_and_continue;
		}
		/* domain is valid, get info from this APQN */
		rc = ap_queue_info(qid, &hwinfo, &decfg, &chkstop);
		switch (rc) {
		case -1:
			if (dev) {
				AP_DBF_INFO("%s(%d,%d) queue_info() failed, rm queue dev\n",
					    __func__, ac->id, dom);
				device_unregister(dev);
			}
			fallthrough;
		case 0:
			goto put_dev_and_continue;
		default:
			break;
		}
		/* if no queue device exists, create a new one */
		if (!aq) {
			aq = ap_queue_create(qid, ac);
			if (!aq) {
				AP_DBF_WARN("%s(%d,%d) ap_queue_create() failed\n",
					    __func__, ac->id, dom);
				continue;
			}
			aq->config = !decfg;
			aq->chkstop = chkstop;
			aq->se_bstate = hwinfo.bs;
			dev = &aq->ap_dev.device;
			dev->bus = &ap_bus_type;
			dev->parent = &ac->ap_dev.device;
			dev_set_name(dev, "%02x.%04x", ac->id, dom);
			/* register queue device */
			rc = device_register(dev);
			if (rc) {
				AP_DBF_WARN("%s(%d,%d) device_register() failed\n",
					    __func__, ac->id, dom);
				goto put_dev_and_continue;
			}
			/* get it and thus adjust reference counter */
			get_device(dev);
			if (decfg) {
				AP_DBF_INFO("%s(%d,%d) new (decfg) queue dev created\n",
					    __func__, ac->id, dom);
			} else if (chkstop) {
				AP_DBF_INFO("%s(%d,%d) new (chkstop) queue dev created\n",
					    __func__, ac->id, dom);
			} else {
				/* nudge the queue's state machine */
				ap_queue_init_state(aq);
				AP_DBF_INFO("%s(%d,%d) new queue dev created\n",
					    __func__, ac->id, dom);
			}
			goto put_dev_and_continue;
		}
		/* handle state changes on already existing queue device */
		spin_lock_bh(&aq->lock);
		/* SE bind state */
		aq->se_bstate = hwinfo.bs;
		/* checkstop state */
		if (chkstop && !aq->chkstop) {
			/* checkstop on */
			aq->chkstop = true;
			if (aq->dev_state > AP_DEV_STATE_UNINITIATED) {
				aq->dev_state = AP_DEV_STATE_ERROR;
				aq->last_err_rc = AP_RESPONSE_CHECKSTOPPED;
			}
			spin_unlock_bh(&aq->lock);
			pr_debug("(%d,%d) queue dev checkstop on\n",
				 ac->id, dom);
			/* 'receive' pending messages with -EAGAIN */
			ap_flush_queue(aq);
			goto put_dev_and_continue;
		} else if (!chkstop && aq->chkstop) {
			/* checkstop off */
			aq->chkstop = false;
			if (aq->dev_state > AP_DEV_STATE_UNINITIATED)
				_ap_queue_init_state(aq);
			spin_unlock_bh(&aq->lock);
			pr_debug("(%d,%d) queue dev checkstop off\n",
				 ac->id, dom);
			goto put_dev_and_continue;
		}
		/* config state change */
		if (decfg && aq->config) {
			/* config off this queue device */
			aq->config = false;
			if (aq->dev_state > AP_DEV_STATE_UNINITIATED) {
				aq->dev_state = AP_DEV_STATE_ERROR;
				aq->last_err_rc = AP_RESPONSE_DECONFIGURED;
			}
			spin_unlock_bh(&aq->lock);
			pr_debug("(%d,%d) queue dev config off\n",
				 ac->id, dom);
			ap_send_config_uevent(&aq->ap_dev, aq->config);
			/* 'receive' pending messages with -EAGAIN */
			ap_flush_queue(aq);
			goto put_dev_and_continue;
		} else if (!decfg && !aq->config) {
			/* config on this queue device */
			aq->config = true;
			if (aq->dev_state > AP_DEV_STATE_UNINITIATED)
				_ap_queue_init_state(aq);
			spin_unlock_bh(&aq->lock);
			pr_debug("(%d,%d) queue dev config on\n",
				 ac->id, dom);
			ap_send_config_uevent(&aq->ap_dev, aq->config);
			goto put_dev_and_continue;
		}
		/* handle other error states */
		if (!decfg && aq->dev_state == AP_DEV_STATE_ERROR) {
			spin_unlock_bh(&aq->lock);
			/* 'receive' pending messages with -EAGAIN */
			ap_flush_queue(aq);
			/* re-init (with reset) the queue device */
			ap_queue_init_state(aq);
			AP_DBF_INFO("%s(%d,%d) queue dev reinit enforced\n",
				    __func__, ac->id, dom);
			goto put_dev_and_continue;
		}
		spin_unlock_bh(&aq->lock);
put_dev_and_continue:
		put_device(dev);
	}
}

/*
 * Helper function for ap_scan_bus().
 * Does the scan bus job for the given adapter id.
 */
static inline void ap_scan_adapter(int ap)
{
	struct ap_tapq_hwinfo hwinfo;
	int rc, dom, comp_type;
	bool decfg, chkstop;
	struct ap_card *ac;
	struct device *dev;
	ap_qid_t qid;

	/* Is there currently a card device for this adapter ? */
	dev = bus_find_device(&ap_bus_type, NULL,
			      (void *)(long)ap,
			      __match_card_device_with_id);
	ac = dev ? to_ap_card(dev) : NULL;

	/* Adapter not in configuration ? */
	if (!ap_test_config_card_id(ap)) {
		if (ac) {
			AP_DBF_INFO("%s(%d) ap not in config any more, rm card and queue devs\n",
				    __func__, ap);
			ap_scan_rm_card_dev_and_queue_devs(ac);
			put_device(dev);
		}
		return;
	}

	/*
	 * Adapter ap is valid in the current configuration. So do some checks:
	 * If no card device exists, build one. If a card device exists, check
	 * for type and functions changed. For all this we need to find a valid
	 * APQN first.
	 */

	for (dom = 0; dom <= ap_max_domain_id; dom++)
		if (ap_test_config_usage_domain(dom)) {
			qid = AP_MKQID(ap, dom);
			if (ap_queue_info(qid, &hwinfo, &decfg, &chkstop) > 0)
				break;
		}
	if (dom > ap_max_domain_id) {
		/* Could not find one valid APQN for this adapter */
		if (ac) {
			AP_DBF_INFO("%s(%d) no type info (no APQN found), rm card and queue devs\n",
				    __func__, ap);
			ap_scan_rm_card_dev_and_queue_devs(ac);
			put_device(dev);
		} else {
			pr_debug("(%d) no type info (no APQN found), ignored\n",
				 ap);
		}
		return;
	}
	if (!hwinfo.at) {
		/* No apdater type info available, an unusable adapter */
		if (ac) {
			AP_DBF_INFO("%s(%d) no valid type (0) info, rm card and queue devs\n",
				    __func__, ap);
			ap_scan_rm_card_dev_and_queue_devs(ac);
			put_device(dev);
		} else {
			pr_debug("(%d) no valid type (0) info, ignored\n", ap);
		}
		return;
	}
	hwinfo.value &= TAPQ_CARD_HWINFO_MASK; /* filter card specific hwinfo */
	if (ac) {
		/* Check APQN against existing card device for changes */
		if (ac->hwinfo.at != hwinfo.at) {
			AP_DBF_INFO("%s(%d) hwtype %d changed, rm card and queue devs\n",
				    __func__, ap, hwinfo.at);
			ap_scan_rm_card_dev_and_queue_devs(ac);
			put_device(dev);
			ac = NULL;
		} else if (ac->hwinfo.fac != hwinfo.fac) {
			AP_DBF_INFO("%s(%d) functions 0x%08x changed, rm card and queue devs\n",
				    __func__, ap, hwinfo.fac);
			ap_scan_rm_card_dev_and_queue_devs(ac);
			put_device(dev);
			ac = NULL;
		} else {
			/* handle checkstop state change */
			if (chkstop && !ac->chkstop) {
				/* checkstop on */
				ac->chkstop = true;
				AP_DBF_INFO("%s(%d) card dev checkstop on\n",
					    __func__, ap);
			} else if (!chkstop && ac->chkstop) {
				/* checkstop off */
				ac->chkstop = false;
				AP_DBF_INFO("%s(%d) card dev checkstop off\n",
					    __func__, ap);
			}
			/* handle config state change */
			if (decfg && ac->config) {
				ac->config = false;
				AP_DBF_INFO("%s(%d) card dev config off\n",
					    __func__, ap);
				ap_send_config_uevent(&ac->ap_dev, ac->config);
			} else if (!decfg && !ac->config) {
				ac->config = true;
				AP_DBF_INFO("%s(%d) card dev config on\n",
					    __func__, ap);
				ap_send_config_uevent(&ac->ap_dev, ac->config);
			}
		}
	}

	if (!ac) {
		/* Build a new card device */
		comp_type = ap_get_compatible_type(qid, hwinfo.at, hwinfo.fac);
		if (!comp_type) {
			AP_DBF_WARN("%s(%d) type %d, can't get compatibility type\n",
				    __func__, ap, hwinfo.at);
			return;
		}
		ac = ap_card_create(ap, hwinfo, comp_type);
		if (!ac) {
			AP_DBF_WARN("%s(%d) ap_card_create() failed\n",
				    __func__, ap);
			return;
		}
		ac->config = !decfg;
		ac->chkstop = chkstop;
		dev = &ac->ap_dev.device;
		dev->bus = &ap_bus_type;
		dev->parent = ap_root_device;
		dev_set_name(dev, "card%02x", ap);
		/* maybe enlarge ap_max_msg_size to support this card */
		if (ac->maxmsgsize > atomic_read(&ap_max_msg_size)) {
			atomic_set(&ap_max_msg_size, ac->maxmsgsize);
			AP_DBF_INFO("%s(%d) ap_max_msg_size update to %d byte\n",
				    __func__, ap,
				    atomic_read(&ap_max_msg_size));
		}
		/* Register the new card device with AP bus */
		rc = device_register(dev);
		if (rc) {
			AP_DBF_WARN("%s(%d) device_register() failed\n",
				    __func__, ap);
			put_device(dev);
			return;
		}
		/* get it and thus adjust reference counter */
		get_device(dev);
		if (decfg)
			AP_DBF_INFO("%s(%d) new (decfg) card dev type=%d func=0x%08x created\n",
				    __func__, ap, hwinfo.at, hwinfo.fac);
		else if (chkstop)
			AP_DBF_INFO("%s(%d) new (chkstop) card dev type=%d func=0x%08x created\n",
				    __func__, ap, hwinfo.at, hwinfo.fac);
		else
			AP_DBF_INFO("%s(%d) new card dev type=%d func=0x%08x created\n",
				    __func__, ap, hwinfo.at, hwinfo.fac);
	}

	/* Verify the domains and the queue devices for this card */
	ap_scan_domains(ac);

	/* release the card device */
	put_device(&ac->ap_dev.device);
}

/**
 * ap_get_configuration - get the host AP configuration
 *
 * Stores the host AP configuration information returned from the previous call
 * to Query Configuration Information (QCI), then retrieves and stores the
 * current AP configuration returned from QCI.
 *
 * Return: true if the host AP configuration changed between calls to QCI;
 * otherwise, return false.
 */
static bool ap_get_configuration(void)
{
	if (!ap_qci_info->flags)	/* QCI not supported */
		return false;

	memcpy(ap_qci_info_old, ap_qci_info, sizeof(*ap_qci_info));
	ap_qci(ap_qci_info);

	return memcmp(ap_qci_info, ap_qci_info_old,
		      sizeof(struct ap_config_info)) != 0;
}

/*
 * ap_config_has_new_aps - Check current against old qci info if
 * new adapters have appeared. Returns true if at least one new
 * adapter in the apm mask is showing up. Existing adapters or
 * receding adapters are not counted.
 */
static bool ap_config_has_new_aps(void)
{

	unsigned long m[BITS_TO_LONGS(AP_DEVICES)];

	if (!ap_qci_info->flags)
		return false;

	bitmap_andnot(m, (unsigned long *)ap_qci_info->apm,
		      (unsigned long *)ap_qci_info_old->apm, AP_DEVICES);
	if (!bitmap_empty(m, AP_DEVICES))
		return true;

	return false;
}

/*
 * ap_config_has_new_doms - Check current against old qci info if
 * new (usage) domains have appeared. Returns true if at least one
 * new domain in the aqm mask is showing up. Existing domains or
 * receding domains are not counted.
 */
static bool ap_config_has_new_doms(void)
{
	unsigned long m[BITS_TO_LONGS(AP_DOMAINS)];

	if (!ap_qci_info->flags)
		return false;

	bitmap_andnot(m, (unsigned long *)ap_qci_info->aqm,
		      (unsigned long *)ap_qci_info_old->aqm, AP_DOMAINS);
	if (!bitmap_empty(m, AP_DOMAINS))
		return true;

	return false;
}

/**
 * ap_scan_bus(): Scan the AP bus for new devices
 * Always run under mutex ap_scan_bus_mutex protection
 * which needs to get locked/unlocked by the caller!
 * Returns true if any config change has been detected
 * during the scan, otherwise false.
 */
static bool ap_scan_bus(void)
{
	bool config_changed;
	int ap;

	pr_debug(">\n");

	/* (re-)fetch configuration via QCI */
	config_changed = ap_get_configuration();
	if (config_changed) {
		if (ap_config_has_new_aps() || ap_config_has_new_doms()) {
			/*
			 * Appearance of new adapters and/or domains need to
			 * build new ap devices which need to get bound to an
			 * device driver. Thus reset the APQN bindings complete
			 * completion.
			 */
			reinit_completion(&ap_apqn_bindings_complete);
		}
		/* post a config change notify */
		notify_config_changed();
	}
	ap_select_domain();

	/* loop over all possible adapters */
	for (ap = 0; ap <= ap_max_adapter_id; ap++)
		ap_scan_adapter(ap);

	/* scan complete notify */
	if (config_changed)
		notify_scan_complete();

	/* check if there is at least one queue available with default domain */
	if (ap_domain_index >= 0) {
		struct device *dev =
			bus_find_device(&ap_bus_type, NULL,
					(void *)(long)ap_domain_index,
					__match_queue_device_with_queue_id);
		if (dev)
			put_device(dev);
		else
			AP_DBF_INFO("%s no queue device with default domain %d available\n",
				    __func__, ap_domain_index);
	}

	if (atomic64_inc_return(&ap_scan_bus_count) == 1) {
		pr_debug("init scan complete\n");
		ap_send_init_scan_done_uevent();
	}

	ap_check_bindings_complete();

	mod_timer(&ap_scan_bus_timer, jiffies + ap_scan_bus_time * HZ);

	pr_debug("< config_changed=%d\n", config_changed);

	return config_changed;
}

/*
 * Callback for the ap_scan_bus_timer
 * Runs periodically, workqueue timer (ap_scan_bus_time)
 */
static void ap_scan_bus_timer_callback(struct timer_list *unused)
{
	/*
	 * schedule work into the system long wq which when
	 * the work is finally executed, calls the AP bus scan.
	 */
	queue_work(system_long_wq, &ap_scan_bus_work);
}

/*
 * Callback for the ap_scan_bus_work
 */
static void ap_scan_bus_wq_callback(struct work_struct *unused)
{
	/*
	 * Try to invoke an ap_scan_bus(). If the mutex acquisition
	 * fails there is currently another task already running the
	 * AP scan bus and there is no need to wait and re-trigger the
	 * scan again. Please note at the end of the scan bus function
	 * the AP scan bus timer is re-armed which triggers then the
	 * ap_scan_bus_timer_callback which enqueues a work into the
	 * system_long_wq which invokes this function here again.
	 */
	if (mutex_trylock(&ap_scan_bus_mutex)) {
		ap_scan_bus_task = current;
		ap_scan_bus_result = ap_scan_bus();
		ap_scan_bus_task = NULL;
		mutex_unlock(&ap_scan_bus_mutex);
	}
}

static inline void __exit ap_async_exit(void)
{
	if (ap_thread_flag)
		ap_poll_thread_stop();
	chsc_notifier_unregister(&ap_bus_nb);
	cancel_work(&ap_scan_bus_work);
	hrtimer_cancel(&ap_poll_timer);
	timer_delete(&ap_scan_bus_timer);
}

static inline int __init ap_async_init(void)
{
	int rc;

	/* Setup the AP bus rescan timer. */
	timer_setup(&ap_scan_bus_timer, ap_scan_bus_timer_callback, 0);

	/*
	 * Setup the high resolution poll timer.
	 * If we are running under z/VM adjust polling to z/VM polling rate.
	 */
	if (machine_is_vm())
		poll_high_timeout = 1500000;
	hrtimer_setup(&ap_poll_timer, ap_poll_timeout, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);

	queue_work(system_long_wq, &ap_scan_bus_work);

	rc = chsc_notifier_register(&ap_bus_nb);
	if (rc)
		goto out;

	/* Start the low priority AP bus poll thread. */
	if (!ap_thread_flag)
		return 0;

	rc = ap_poll_thread_start();
	if (rc)
		goto out_notifier;

	return 0;

out_notifier:
	chsc_notifier_unregister(&ap_bus_nb);
out:
	cancel_work(&ap_scan_bus_work);
	hrtimer_cancel(&ap_poll_timer);
	timer_delete(&ap_scan_bus_timer);
	return rc;
}

static inline void ap_irq_exit(void)
{
	if (ap_irq_flag)
		unregister_adapter_interrupt(&ap_airq);
}

static inline int __init ap_irq_init(void)
{
	int rc;

	if (!ap_interrupts_available() || !ap_useirq)
		return 0;

	rc = register_adapter_interrupt(&ap_airq);
	ap_irq_flag = (rc == 0);

	return rc;
}

static inline void ap_debug_exit(void)
{
	debug_unregister(ap_dbf_info);
}

static inline int __init ap_debug_init(void)
{
	ap_dbf_info = debug_register("ap", 2, 1,
				     AP_DBF_MAX_SPRINTF_ARGS * sizeof(long));
	debug_register_view(ap_dbf_info, &debug_sprintf_view);
	debug_set_level(ap_dbf_info, DBF_ERR);

	return 0;
}

static void __init ap_perms_init(void)
{
	/* all resources usable if no kernel parameter string given */
	memset(&ap_perms.ioctlm, 0xFF, sizeof(ap_perms.ioctlm));
	memset(&ap_perms.apm, 0xFF, sizeof(ap_perms.apm));
	memset(&ap_perms.aqm, 0xFF, sizeof(ap_perms.aqm));

	/* apm kernel parameter string */
	if (apm_str) {
		memset(&ap_perms.apm, 0, sizeof(ap_perms.apm));
		ap_parse_mask_str(apm_str, ap_perms.apm, AP_DEVICES,
				  &ap_perms_mutex);
	}

	/* aqm kernel parameter string */
	if (aqm_str) {
		memset(&ap_perms.aqm, 0, sizeof(ap_perms.aqm));
		ap_parse_mask_str(aqm_str, ap_perms.aqm, AP_DOMAINS,
				  &ap_perms_mutex);
	}
}

/**
 * ap_module_init(): The module initialization code.
 *
 * Initializes the module.
 */
static int __init ap_module_init(void)
{
	int rc;

	rc = ap_debug_init();
	if (rc)
		return rc;

	if (!ap_instructions_available()) {
		pr_warn("The hardware system does not support AP instructions\n");
		return -ENODEV;
	}

	/* init ap_queue hashtable */
	hash_init(ap_queues);

	/* create ap msg buffer memory pool */
	ap_msg_pool = mempool_create_kmalloc_pool(ap_msg_pool_min_items,
						  AP_DEFAULT_MAX_MSG_SIZE);
	if (!ap_msg_pool) {
		rc = -ENOMEM;
		goto out;
	}

	/* set up the AP permissions (ioctls, ap and aq masks) */
	ap_perms_init();

	/* Get AP configuration data if available */
	ap_init_qci_info();

	/* check default domain setting */
	if (ap_domain_index < -1 || ap_domain_index > ap_max_domain_id ||
	    (ap_domain_index >= 0 &&
	     !test_bit_inv(ap_domain_index, ap_perms.aqm))) {
		pr_warn("%d is not a valid cryptographic domain\n",
			ap_domain_index);
		ap_domain_index = -1;
	}

	/* Create /sys/bus/ap. */
	rc = bus_register(&ap_bus_type);
	if (rc)
		goto out;

	/* Create /sys/devices/ap. */
	ap_root_device = root_device_register("ap");
	rc = PTR_ERR_OR_ZERO(ap_root_device);
	if (rc)
		goto out_bus;
	ap_root_device->bus = &ap_bus_type;

	/* enable interrupts if available */
	rc = ap_irq_init();
	if (rc)
		goto out_device;

	/* Setup asynchronous work (timers, workqueue, etc). */
	rc = ap_async_init();
	if (rc)
		goto out_irq;

	return 0;

out_irq:
	ap_irq_exit();
out_device:
	root_device_unregister(ap_root_device);
out_bus:
	bus_unregister(&ap_bus_type);
out:
	mempool_destroy(ap_msg_pool);
	ap_debug_exit();
	return rc;
}

static void __exit ap_module_exit(void)
{
	ap_async_exit();
	ap_irq_exit();
	root_device_unregister(ap_root_device);
	bus_unregister(&ap_bus_type);
	mempool_destroy(ap_msg_pool);
	ap_debug_exit();
}

module_init(ap_module_init);
module_exit(ap_module_exit);
