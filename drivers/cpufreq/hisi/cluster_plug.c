/*
 * Cluster-plug CPU Hotplug Driver
 * Designed for homogeneous ARM big.LITTLE systems
 *
 * Copyright (C) 2015-2016 Sultan Qasim Khan and Christopher R. Palmer
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/cpufreq.h>
#include <linux/lcd_notify.h>

#define CLUSTER_PLUG_MAJOR_VERSION	3
#define CLUSTER_PLUG_MINOR_VERSION	0

#define DEF_LOAD_THRESH_DOWN	20
#define DEF_LOAD_THRESH_UP		80
#define DEF_SAMPLING_MS			50
#define DEF_VOTE_THRESHOLD_UP	2
#define DEF_VOTE_THRESHOLD_DOWN	5
#define DEF_MAX_CORES_SCREENOFF 1
#define DEF_ENABLED_BY_DEFAULT  false

#define N_BIG_CPUS				4
#define N_LITTLE_CPUS			4

// configure architecture and behavior of cluster_plug
#define FIGHT_INTERFERENCE // ensure big cluster is enabled (avoid interference of kernel adiutor mod for example)
#define LITTLE_BIG // two variants: big.LITTLE (cpu0=big) and LITTLE.big (cpu0=LITTLE)

// do not forget to disable debugging later
#if 1
#define CLUSTER_PLUG_DEBUG
#endif

#ifdef CLUSTER_PLUG_DEBUG
#define LOGD    pr_info
#else
#define LOGD(...)
#endif /* CLUSTER_PLUG_DEBUG */

static DEFINE_MUTEX(cluster_plug_mutex);
static struct delayed_work cluster_plug_work;
static struct workqueue_struct *clusterplug_wq;

static unsigned int load_threshold_down = DEF_LOAD_THRESH_DOWN;
module_param(load_threshold_down, uint, 0664);

static unsigned int load_threshold_up = DEF_LOAD_THRESH_UP;
module_param(load_threshold_up, uint, 0664);

static unsigned int sampling_time = DEF_SAMPLING_MS;
module_param(sampling_time, uint, 0664);

static unsigned int vote_threshold_down = DEF_VOTE_THRESHOLD_DOWN;
module_param(vote_threshold_down, uint, 0664);

static unsigned int vote_threshold_up = DEF_VOTE_THRESHOLD_UP;
module_param(vote_threshold_up, uint, 0664);

static unsigned int max_cores_screenoff = DEF_MAX_CORES_SCREENOFF;
module_param(max_cores_screenoff, uint, 0664);

static bool active = DEF_ENABLED_BY_DEFAULT;
static bool workqueue_suspended = true;

static bool screen_on = true;
static bool big_cluster_enabled = true;
static bool little_cluster_enabled = true;
static bool low_power_mode = false;
static bool screen_off_power_mode = false;
static bool online_all = false;

static unsigned int vote_up = 0;
static unsigned int vote_down = 0;

struct cp_cpu_info {
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
};

static DEFINE_PER_CPU(struct cp_cpu_info, cp_info);

bool isClusterPlugEnabled(void) {
	return active;
}

static bool is_big_cpu(unsigned int cpu)
{
#ifdef LITTLE_BIG
	return cpu >= N_LITTLE_CPUS;
#else
	return cpu < N_BIG_CPUS;
#endif
}

static bool is_little_cpu(unsigned int cpu)
{
#ifdef LITTLE_BIG
	return cpu < N_LITTLE_CPUS;
#else
	return !is_big_cpu(cpu);
#endif
}

static unsigned int get_delta_cpu_load_and_update(unsigned int cpu)
{
	u64 cur_wall_time, cur_idle_time;
	unsigned int wall_time, idle_time;
	struct cp_cpu_info *l_cp_info = &per_cpu(cp_info, cpu);

	/* last parameter 0 means that IO wait is considered idle */
	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, 0);

	wall_time = (unsigned int)
		(cur_wall_time - l_cp_info->prev_cpu_wall);
	l_cp_info->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int)
		(cur_idle_time - l_cp_info->prev_cpu_idle);
	l_cp_info->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 100;
	else
		return 100 * (wall_time - idle_time) / wall_time;
}

static void __ref optimized_online_all(void) {
	struct device *cpu_dev;
	int cpu_num;

	for (cpu_num = 7; cpu_num >= 0; cpu_num--) {
		if (cpu_is_offline(cpu_num)) {
			cpu_dev = get_cpu_device(cpu_num);
			device_lock(cpu_dev);
			cpu_up(cpu_num);
			/* update offline manually */
			cpu_dev->offline = false;
			device_unlock(cpu_dev);
		}
	}
}

static void __ref switch_cpu(unsigned int cpu, bool offline) {
	struct device *cpu_dev;

	cpu_dev = get_cpu_device(cpu);
	device_lock(cpu_dev);
	if (offline) {
		if (cpu_online(cpu))
			cpu_down(cpu);
	} else {
		if (cpu_is_offline(cpu))
			cpu_up(cpu);
	}

	/* update offline manually */
	cpu_dev->offline = offline;
	device_unlock(cpu_dev);
}

static void disable_big_cluster(void)
{
	unsigned int cpu;
	unsigned int num_down = 0;

	if (!big_cluster_enabled)
		return;

	for_each_present_cpu(cpu) {
		if (is_big_cpu(cpu) && cpu_online(cpu)) {
			switch_cpu(cpu, true);
			num_down++;
		}
	}

	LOGD("cluster_plug: %d big cpus disabled\n", num_down);

	big_cluster_enabled = false;
}

static void __ref enable_little_cluster(void)
{
	unsigned int cpu;
	unsigned int num_up = 0;
	unsigned int required_cpus = 4;
	unsigned int total_cpus = 0;
	if (screen_on == false && screen_off_power_mode == false) {
		if (max_cores_screenoff > 0 && max_cores_screenoff <= 4)
			required_cpus = max_cores_screenoff;
	}

	for_each_present_cpu(cpu) {
		if (is_little_cpu(cpu)) {
			// do we need more cpu's?
			if (required_cpus > 0) {
				// enable offline or count already online cpu
				if (!cpu_online(cpu)) {
					switch_cpu(cpu, false);
					num_up++;
				}
				required_cpus--;
				total_cpus++;
			} else {
				// disable cpu if online
				if (cpu_online(cpu)) {
					switch_cpu(cpu, true);
				}
			}
		}
	}

	LOGD("cluster_plug: %d little cpus enabled, %d little cpus active\n", num_up, total_cpus);

	little_cluster_enabled = true;
}

static void queue_clusterplug_work(unsigned ms)
{
	queue_delayed_work(clusterplug_wq, &cluster_plug_work, msecs_to_jiffies(ms));
}

static void __ref online_one_cpu(void) {
	struct device *cpu_dev;
	int cpu_num;

	// start with big cpus, go down to litte cpus but never touch one big and one little cpu (the ones with cpufreq node - on honor 6x these are 0 and 7
	// (enable 1 big, 1 litte -> load increases -> 2 big 1 litte ... 4 big 1 little .. 4 big 4 little) ?
#ifdef LITTLE_BIG
	for (cpu_num = 7-1; cpu_num >= 0+1; cpu_num--) {
#else
	for (cpu_num = 0+1; cpu_num <= 7-1; cpu_num++) {
#endif
		if (cpu_is_offline(cpu_num)) {
			cpu_dev = get_cpu_device(cpu_num);
			device_lock(cpu_dev);
			cpu_up(cpu_num);
			/* update offline manually */
			cpu_dev->offline = false;
			device_unlock(cpu_dev);
			LOGD("cluster_plug: cpu %d switched on\n", cpu_num);
			return;
		}
	}
}

static void __ref offline_one_cpu(void) {
	struct device *cpu_dev;
	int cpu_num;

	// start with big cpus, go down to litte cpus but never touch one big and one little cpu (the ones with cpufreq node - on honor 6x these are 0 and 7
	// (enable 1 big, 1 litte -> load increases -> 2 big 1 litte ... 4 big 1 little .. 4 big 4 little) ?
#ifdef LITTLE_BIG
	for (cpu_num = 7-1; cpu_num >= 0+1; cpu_num--) {
#else
	for (cpu_num = 0+1; cpu_num <= 7-1; cpu_num++) {
#endif
		if (cpu_online(cpu_num)) {
			cpu_dev = get_cpu_device(cpu_num);
			device_lock(cpu_dev);
			cpu_down(cpu_num);
			/* update offline manually */
			cpu_dev->offline = true;
			device_unlock(cpu_dev);
			LOGD("cluster_plug: cpu %d switched off\n", cpu_num);
			return;
		}
	}
}

static int calculateLoad(void)
{
	unsigned int cpu, cpus = 0;
	unsigned int cpu_load_little = 0;
	unsigned int cpu_load_big = 0;
	unsigned int load, load_avg;

	for_each_possible_cpu(cpu) {
		
		unsigned int cpu_load = get_delta_cpu_load_and_update(cpu);
		bool online = cpu_online(cpu);
		LOGD("cluster_plug: load CPU: %d Online: %d Load: %d\n", cpu, online, cpu_load);
		if (online) {
			cpus++;
			if (is_big_cpu(cpu)) {
				cpu_load_big += cpu_load;
			} else {
				cpu_load_little += cpu_load;
			}
		}
	}
	
	load = cpu_load_big + cpu_load_little;
	load_avg = load / cpus;
	
	LOGD("cluster_plug: load AVG on %d cpus %d\n", cpus, load_avg);
	LOGD("cluster_plug: load SUM %d\n", load);
	LOGD("cluster_plug: load BIG %d\n", cpu_load_big);
	LOGD("cluster_plug: load LIT %d\n", cpu_load_little);
	
	return load_avg;
}

static void __ref cluster_plug_perform(void) {
	int load = calculateLoad();
	
	if (load > load_threshold_up) {
		vote_up++;
		if (vote_down > 0) vote_down--;
	} else if (load < load_threshold_down) {
		vote_down++;
		if (vote_up > 0) vote_up--;
	}

	LOGD("cluster_plug: vote_up   %d\n", vote_up);
	LOGD("cluster_plug: vote_down %d\n", vote_down);
	
	if (vote_up > vote_threshold_up) {
		// online another cpu if possible
		online_one_cpu();

		vote_up = vote_down = 0;
	} else if (vote_down > vote_threshold_down) {
		// offline another cpu if possible
		offline_one_cpu();

		vote_up = vote_down = 0;
	}
}

static void __ref cluster_plug_work_fn(struct work_struct *work)
{
	if (online_all) {
		// used for first start after screen on (fingerprint boost)
		optimized_online_all();
		big_cluster_enabled = true;
		little_cluster_enabled = true;
		LOGD("cluster_plug: all cpus enabled\n");
	}

	if (active) {
		// in online_all mode we wait for next run to do anything except online_all
		if (!online_all) {
			if (
				(screen_on || screen_off_power_mode == true) && // work to do because mode matches
				low_power_mode == false // no work to do because fixed little cpuset for all screen states
			) {
				cluster_plug_perform();
			} else {
				// low_power_mode or screen_off without screen_off_power_mode
				enable_little_cluster();
				disable_big_cluster();

				// Do not schedule more work 
				mutex_lock(&cluster_plug_mutex);
				workqueue_suspended = true;
				mutex_unlock(&cluster_plug_mutex);

				return;
			}
		}
		
		LOGD("cluster_plug: queue_clusterplug_work(%d)\n", sampling_time);
		queue_clusterplug_work(sampling_time);
	}

	online_all = false;
}
static void __ref cluster_plug_hotplug_suspend(void)
{
	if (active) {
		LOGD("cluster_plug: cluster_plug_hotplug_suspend called\n");
		screen_on = false;

		// required to reflect screen off changes if cluster_plug is in simple mode without regular work
		if (workqueue_suspended) {
			// restart work queue
			workqueue_suspended = false;

			queue_clusterplug_work(1);
		}

		LOGD("cluster_plug: cluster_plug_hotplug_suspend finished\n");
	}
}

static void __ref cluster_plug_hotplug_resume(void)
{
	if (active) {
		LOGD("cluster_plug: cluster_plug_hotplug_resume called\n");
		screen_on = true;

		// required to reflect screen on changes because cluster_plug is always in simple mode without regular work while screen off
		if (workqueue_suspended) {
			// restart work queue

			/* make the internal state match the actual state */
			// and most important online all cpus to boost fingerprint
			online_all = true;

			// reset voting
			vote_up = vote_down = 0;

			workqueue_suspended = false;

			// queue_clusterplug_work(1);
			queue_work(clusterplug_wq, &cluster_plug_work);
		}

		LOGD("cluster_plug: cluster_plug_hotplug_resume finished\n");
	}
}

static struct notifier_block lcd_notif;

static int lcd_notifier_callback(struct notifier_block *this,
								unsigned long event, void *data)
{
	if (!active)
		return NOTIFY_OK;

	switch (event)
	{
		// we use OFF_END because shutdown of cpus is no priority task and ON_START to get earliest possible notification to fire up cpus and so boost fingerprint
		case LCD_EVENT_OFF_START:
			LOGD("cluster_plug: LCD_EVENT_OFF_START\n");
			break;

		case LCD_EVENT_OFF_END:
			LOGD("cluster_plug: LCD_EVENT_OFF_END\n");
			mutex_lock(&cluster_plug_mutex);
			cluster_plug_hotplug_suspend();
			mutex_unlock(&cluster_plug_mutex);
			break;

		case LCD_EVENT_ON_START:
			// display might turn on - caused by fingerprint or button press
			LOGD("cluster_plug: LCD_EVENT_ON_START\n");
			mutex_lock(&cluster_plug_mutex);
			cluster_plug_hotplug_resume();
			mutex_unlock(&cluster_plug_mutex);
			break;

		case LCD_EVENT_ON_END:
			LOGD("cluster_plug: LCD_EVENT_ON_END\n");
			break;

		default:
			break;
	}

	return NOTIFY_OK;
}

static int __ref active_show(char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	return snprintf(buf, PAGE_SIZE, "%d", active);
}

static int __ref active_store(const char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret == 0) {
		if ((value != 0) == active)
			return ret;

		mutex_lock(&cluster_plug_mutex);

		flush_workqueue(clusterplug_wq);
		cancel_delayed_work(&cluster_plug_work);

		active = (value != 0);

		/* make the internal state match the actual state */
		online_all = true;

		workqueue_suspended = false;
		queue_clusterplug_work(1);

		mutex_unlock(&cluster_plug_mutex);
	}

	return ret;
}

static const struct kernel_param_ops param_ops_active = {
	.set = active_store,
	.get = active_show
};

module_param_cb(active, &param_ops_active, &active, 0664);

static int __ref low_power_mode_show(char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	return snprintf(buf, PAGE_SIZE, "%d", low_power_mode);
}

static int __ref low_power_mode_store(const char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret == 0) {
		if ((value != 0) == low_power_mode)
			return ret;

		mutex_lock(&cluster_plug_mutex);

		flush_workqueue(clusterplug_wq);
		cancel_delayed_work(&cluster_plug_work);

		low_power_mode = (value != 0);
		if (low_power_mode) {
			screen_off_power_mode = false;
		}

		workqueue_suspended = false;
		queue_clusterplug_work(1);

		mutex_unlock(&cluster_plug_mutex);
	}

	return ret;
}

static const struct kernel_param_ops param_ops_low_power_mode = {
	.set = low_power_mode_store,
	.get = low_power_mode_show
};

module_param_cb(low_power_mode, &param_ops_low_power_mode, &low_power_mode, 0664);

static int __ref screen_off_power_mode_show(char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	return snprintf(buf, PAGE_SIZE, "%d", screen_off_power_mode);
}

static int __ref screen_off_power_mode_store(const char *buf,
			const struct kernel_param *kp __attribute__ ((unused)))
{
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret == 0) {
		if ((value != 0) == screen_off_power_mode)
			return ret;

		mutex_lock(&cluster_plug_mutex);

		flush_workqueue(clusterplug_wq);
		cancel_delayed_work(&cluster_plug_work);

		screen_off_power_mode = (value != 0);
		if (screen_off_power_mode) {
			low_power_mode = false;
		}

		workqueue_suspended = false;
		queue_clusterplug_work(1);

		mutex_unlock(&cluster_plug_mutex);
	}

	return ret;
}

static const struct kernel_param_ops param_ops_screen_off_power_mode = {
	.set = screen_off_power_mode_store,
	.get = screen_off_power_mode_show
};

module_param_cb(screen_off_power_mode, &param_ops_screen_off_power_mode, &screen_off_power_mode, 0664);

int __init cluster_plug_init(void)
{
	pr_info("cluster_plug: version %d.%d by sultanqasim and crpalmer\n",
		 CLUSTER_PLUG_MAJOR_VERSION,
		 CLUSTER_PLUG_MINOR_VERSION);

	clusterplug_wq = alloc_workqueue("clusterplug",
				WQ_HIGHPRI | WQ_UNBOUND, 1);

	lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&lcd_notif) != 0)
	{
		pr_err("%s: Failed to register lcd callback\n", __func__);

		destroy_workqueue(clusterplug_wq);

		return -EFAULT;
	}

	INIT_DELAYED_WORK(&cluster_plug_work, cluster_plug_work_fn);

	pr_info("%s cluster plug initialisation complete\n", __FUNCTION__);

	return 0;
}

static void __exit cluster_plug_exit(void)
{
	flush_workqueue(clusterplug_wq);
	cancel_delayed_work_sync(&cluster_plug_work);

	lcd_unregister_client(&lcd_notif);

	destroy_workqueue(clusterplug_wq);

	pr_info("%s cluster_plug unregistration complete\n", __FUNCTION__);
}

bool is_clusterplug_enabled(void)
{
       return active;
}


MODULE_AUTHOR("Sultan Qasim Khan <sultanqasim@gmail.com> and Christopher R. Palmer <crpalmer@gmail.com>");
MODULE_DESCRIPTION("'cluster_plug' - A cluster based hotplug for homogeneous"
        "ARM big.LITTLE systems where the big cluster is preferred."
);
MODULE_LICENSE("GPL");

late_initcall(cluster_plug_init);
module_exit(cluster_plug_exit);
