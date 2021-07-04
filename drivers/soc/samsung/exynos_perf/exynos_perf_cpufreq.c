/*
 * cpufreq logging driver
 * Jungwook Kim <jwook1.kim@samsung.com>
 */

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/thermal.h>
#include <asm/topology.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <soc/samsung/gpu_cooling.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-devfreq.h>
#include "../../../kernel/sched/sched.h"
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <soc/samsung/exynos_pm_qos.h>
#include <trace/events/sched.h>
#include <trace/events/ems_debug.h>
#include <soc/samsung/freq-qos-tracer.h>

#define MAX_CLUSTER 3

#if IS_ENABLED(CONFIG_EXYNOS_UFCC)
extern unsigned int get_cpufreq_max_limit(void);
#endif

#if IS_ENABLED(CONFIG_EXYNOS_AFM)
extern unsigned int get_afm_clipped_freq(int cpu);
#endif

static const char *prefix = "exynos_perf";

#define MAX_FREQ 40
#define MAX_CLUSTER 3

static int cl_cnt = 0;

static uint cal_id_mif = 0;
static uint cal_id_g3d = 0;
static uint devfreq_mif = 0;

static int is_running = 0;
static void *buf = NULL;
static uint polling_ms = 100;
static uint bind_cpu = 0;
static uint pid = 0;
static uint gov = 0;
static uint power_limit = 8000;

static uint cpu_util_avgs[NR_CPUS];

//-----------------------------------
struct my_cpu {
	unsigned int freq;
	unsigned int volt;
	unsigned int power;
};
struct my_cluster {
	struct my_cpu cpus[MAX_FREQ];
	unsigned int maxfreq;
	unsigned int minfreq;
	int first_cpu;
	int last_cpu;
	int coeff;
	unsigned int cal_id;
	struct freq_qos_request freq_qos;
};
static struct my_cluster cls[MAX_CLUSTER];
//-----------------------------------


static int get_cl_idx(int cpu)
{
	int index = 0;
	int i;
	for (i = 0; i < MAX_CLUSTER; i++) {
		if (cpu <= cls[i].last_cpu) {
			index = i;
			break;
		}
	}
	return index;
}

static int get_f_idx(int cl_idx, int freq)
{
	int index = 0;
	int i;
	for (i = 0; i < MAX_FREQ; i++) {
		if (cls[cl_idx].cpus[i].freq == freq) {
			index = i;
			break;
		}
	}
	return index;
}

//---------------------------------------
// thread main
static int cpufreq_log_thread(void *data)
{
	int i;
	int ret = 0;
	gfp_t gfp;
	uint buf_size;
	struct thermal_zone_device *tz;
	int temp[3], temp_g3d;
	const char *tz_names[] = { "BIG", "LITTLE", "MID" };
	uint siop, ocp;
	uint cpu;
	uint cpu_cur[MAX_CLUSTER];
	uint cpu_max[MAX_CLUSTER];
	uint mif, gpu;
	int gpu_util;
	int grp_start, grp_num;
	int cpu_power;
	int power_total;
	int cl_idx;
	int f_idx;
	int freq;
	int next_mgn;
	int try_alloc_cnt = 3;
	struct pid *ppid;
	struct task_struct *tsk;

	if (is_running) {
		pr_info("[%s] already running!!\n", prefix);
		return 0;
	}

	// alloc buf
	gfp = GFP_KERNEL;
	buf_size = KMALLOC_MAX_SIZE;
	if (buf)
		kfree(buf);
	while (try_alloc_cnt-- > 0) {
		buf = kmalloc(buf_size, gfp);
		if (!buf) {
			pr_info("[%s] kmalloc failed: try again...%d\n", prefix, try_alloc_cnt);
			buf_size -= (1024*1024);
			continue;
		} else {
			break;
		}
	}
	if (try_alloc_cnt <= 0) {
		pr_info("[%s] kmalloc failed: buf_size: %d\n", prefix, buf_size);
		return 0;
	} else {
		memset(buf, 0, buf_size);
		pr_info("[%s] kmalloc ok. buf=%p, buf_size=%d\n", prefix, buf, buf_size);
	}

	// start
	is_running = 1;
	pr_info("[%s] cpufreq log start\n", prefix);

	//---------------------
	// header
	//---------------------

	// temperature
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_BIG ");
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_LITTLE ");
	if (cl_cnt > 2)
		ret += snprintf(buf + ret, buf_size - ret, "01-temp_MID ");
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_G3D ");

	// cpufreq
	grp_start = 2;
	for (i = cl_cnt-1; i >= 0; i--) {
		grp_num = grp_start + (cl_cnt-1 - i);
		cpu = cls[i].first_cpu;
		ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_max ", grp_num, cpu);
		ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_cur ", grp_num, cpu);
		if (i == (cl_cnt-1)) { // if big cluster
			ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_siop ", grp_num, cpu);
			ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_ocp ", grp_num, cpu);
		}
	}

	// mif, gpu, task
	ret += snprintf(buf + ret, buf_size - ret, "05-mif_cur 06-gpu_util 06-gpu_cur 07-task_cpu ");

	// cpu util
	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, buf_size - ret, "08-util_cpu%d ", cpu);
	}

	// cpu power
	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, buf_size - ret, "09-power_cpu%d ", cpu);
	}
	ret += snprintf(buf + ret, buf_size - ret, "09-power_total ");
	ret -= 1;
	ret += snprintf(buf + ret, buf_size - ret, "\n");

	// freq_qos
	freq_qos_tracer_add_request(&cpufreq_cpu_get_raw(7)->constraints, &cls[2].freq_qos,
					FREQ_QOS_MAX, INT_MAX);
	freq_qos_tracer_add_request(&cpufreq_cpu_get_raw(4)->constraints, &cls[1].freq_qos,
					FREQ_QOS_MAX, INT_MAX);
	freq_qos_tracer_add_request(&cpufreq_cpu_get_raw(0)->constraints, &cls[0].freq_qos,
					FREQ_QOS_MAX, INT_MAX);

	//---------------------
	// body
	//---------------------
	while (is_running) {
		// cpu temperature
		for (i = 0; i < cl_cnt; i++) {
			tz = thermal_zone_get_zone_by_name(tz_names[i]);
			thermal_zone_get_temp(tz, &temp[i]);
			temp[i] = (temp[i] < 0)? 0 : temp[i] / 1000;
			ret += snprintf(buf + ret, buf_size - ret, "%d ", temp[i]);
		}
		// gpu temperature
		tz = thermal_zone_get_zone_by_name("G3D");
		thermal_zone_get_temp(tz, &temp_g3d);
		temp_g3d = (temp_g3d < 0)? 0 : temp_g3d / 1000;
		ret += snprintf(buf + ret, buf_size - ret, "%d ", temp_g3d);

		// siop, ocp
#if IS_ENABLED(CONFIG_EXYNOS_UFCC)
		siop = get_cpufreq_max_limit() / 1000;
		siop = (siop > 4000)? 0 : siop;
#else
		siop = 0;
#endif
#if IS_ENABLED(CONFIG_EXYNOS_AFM)
		ocp = get_afm_clipped_freq(7) / 1000;
		ocp = (ocp > 4000)? 0 : ocp;
#else
		ocp = 0;
#endif
		// cpufreq
		for (i = cl_cnt - 1; i >= 0; i--) {
			cpu = cls[i].first_cpu;
			cpu_max[i] = cpufreq_quick_get_max(cpu) / 1000;
			cpu_cur[i] = cpufreq_quick_get(cpu) / 1000;
			ret += snprintf(buf + ret, buf_size - ret, "%d %d ", cpu_max[i], cpu_cur[i]);
			if (i == (cl_cnt-1)) {
				ret += snprintf(buf + ret, buf_size - ret, "%d %d ", siop, ocp);
			}
		}
		// mif
		mif = (uint)exynos_devfreq_get_domain_freq(devfreq_mif) / 1000;

		// gpu
		gpu_util = gpu_dvfs_get_utilization();

		//gpu_util = 0;
		gpu = (uint)cal_dfs_cached_get_rate(cal_id_g3d) / 1000;
		ret += snprintf(buf + ret, buf_size - ret, "%d %d %d ", mif, gpu_util, gpu);

		// task
		ppid = find_get_pid(pid);
		tsk = get_pid_task(ppid, PIDTYPE_PID);
                cpu = (!tsk)? 0 : task_cpu(tsk);
		ret += snprintf(buf + ret, buf_size - ret, "%d ", cpu);

		// cpu util
		for_each_possible_cpu(cpu)
			ret += snprintf(buf + ret, buf_size - ret, "%d ", cpu_util_avgs[cpu]);

		// cpu power
		power_total = 0;
		for_each_possible_cpu(cpu) {
			int util_ratio;
			util_ratio = (cpu_util_avgs[cpu] * 100) / 1024;
			cl_idx = get_cl_idx(cpu);
			freq = cpu_cur[cl_idx] * 1000;
			f_idx = get_f_idx(cl_idx, freq);
			//cpu_power = util_ratio * (cls[cl_idx].cpus[f_idx].power / 100) / 100;
			cpu_power = util_ratio * (cls[cl_idx].cpus[f_idx].power / 100);
			power_total += cpu_power;
			ret += snprintf(buf + ret, buf_size - ret, "%d ", cpu_power);
		}
		ret += snprintf(buf + ret, buf_size - ret, "%d ", power_total);
		ret -= 1;
		ret += snprintf(buf + ret, buf_size - ret, "\n");

		// next max lock
		if (gov) {
			next_mgn = (power_total > power_limit)? -1 : 1;		// android r: reverse
			for (cl_idx = 0; cl_idx < MAX_CLUSTER; cl_idx++) {
				freq = cpu_max[cl_idx] * 1000;
				f_idx = get_f_idx(cl_idx, freq);
				f_idx += next_mgn;
				f_idx = (f_idx < 0)? 0 : f_idx;
				freq = cls[cl_idx].cpus[f_idx].freq;
				f_idx = (freq == 0)? f_idx - 1 : f_idx;
				freq_qos_update_request(&cls[cl_idx].freq_qos, cls[cl_idx].cpus[f_idx].freq);
			}
		}

		// check buf size
		if ( ret + 256 > buf_size ) {
			pr_info("[%s] buf_size: %d + 256 > %d!!", prefix, ret, buf_size);
			break;
		}
		msleep(polling_ms);
	}

	// init max freq
	for (cl_idx = 0; cl_idx < MAX_CLUSTER; cl_idx++) {
		freq_qos_update_request(&cls[cl_idx].freq_qos, INT_MAX);
	}

	freq_qos_tracer_remove_request(&cls[0].freq_qos);
	freq_qos_tracer_remove_request(&cls[1].freq_qos);
	freq_qos_tracer_remove_request(&cls[2].freq_qos);

	return 0;
}

static struct task_struct *task;
static void cpufreq_log_start(void)
{
	if (is_running) {
		pr_err("[%s] already running!!\n", prefix);
		return;
	}
	task = kthread_create(cpufreq_log_thread, NULL, "thread%u", 0);
	kthread_bind(task, bind_cpu);
	wake_up_process(task);
	return;
}

static void cpufreq_log_stop(void)
{
	is_running = 0;
	pr_info("[%s] cpufreq profile done\n", prefix);
}

//===========================================================
// sysfs nodes
//===========================================================

// module -> gki hooking function
static void update_cpu_util_avg(void *data, struct cfs_rq *cfs_rq)
{
	unsigned int util = READ_ONCE(cfs_rq->avg.util_avg);
	util = (util > 1024)? 1024 : util;
	cpu_util_avgs[cpu_of(rq_of(cfs_rq))] = util;
}

#define DEF_NODE(name) \
	static ssize_t show_##name(struct kobject *k, struct kobj_attribute *attr, char *buf) { \
		int ret = 0; \
		ret += sprintf(buf + ret, "%d\n", name); \
		return ret; } \
	static ssize_t store_##name(struct kobject *k, struct kobj_attribute *attr, const char *buf, size_t count) { \
		if (sscanf(buf, "%d", &name) != 1) \
			return -EINVAL; \
		return count; } \
	static struct kobj_attribute name##_attr = __ATTR(name, 0644, show_##name, store_##name);

DEF_NODE(polling_ms)
DEF_NODE(bind_cpu)
DEF_NODE(pid)
DEF_NODE(gov)
DEF_NODE(power_limit)

unsigned long freq_table[MAX_FREQ];
unsigned int volt_table[MAX_FREQ];

static int init_cpu_power(void)
{
	struct cpufreq_policy *policy;
	struct cpumask *mask;
	int next_cpu = 0;
	int i, j, cpu, last_cpu;
	int cal_table_size;
	u64 f, v, c, p;

	for (i = 0; i < MAX_CLUSTER; i++)
		memset(cls[i].cpus, 0, sizeof(struct my_cpu)*MAX_FREQ);

//	cls[0].coeff = 2089;
//	cls[1].coeff = 7636;
//	cls[2].coeff = 11912;
	cls[0].coeff = 15;
	cls[1].coeff = 80;
	cls[2].coeff = 110;

	while ( next_cpu < num_possible_cpus() ) {
		policy = cpufreq_cpu_get(next_cpu);
		if (!policy) {
			next_cpu++;
			continue;
		}
		mask = policy->related_cpus;
		for_each_cpu(cpu, mask) {
			last_cpu = cpu;
		}

		cls[cl_cnt].maxfreq = policy->cpuinfo.max_freq;
		cls[cl_cnt].minfreq = policy->cpuinfo.min_freq;

		cal_table_size = cal_dfs_get_lv_num(cls[cl_cnt].cal_id);	// table size
		cal_dfs_get_rate_table(cls[cl_cnt].cal_id, freq_table);	// freq
		cal_dfs_get_asv_table(cls[cl_cnt].cal_id, volt_table);	// voltage
		pr_info("[%s] cal_table_size = %d\n", prefix, cal_table_size);

		j = 0;
		for (i = cal_table_size-1; i >= 0; i--) {
			pr_info("[%s][%d] %d %d\n", prefix, i, cls[cl_cnt].minfreq, freq_table[i]);
			if (cls[cl_cnt].minfreq > freq_table[i])
				continue;
			if (cls[cl_cnt].maxfreq < freq_table[i])
				continue;
			cls[cl_cnt].cpus[j].freq = freq_table[i];
			cls[cl_cnt].cpus[j].volt = volt_table[i];

			f = cls[cl_cnt].cpus[j].freq / 1000;
			v = cls[cl_cnt].cpus[j].volt / 1000;
			c = cls[cl_cnt].coeff;
			p = c * v * v * f;
			p = p / 100000000;
			cls[cl_cnt].cpus[j].power = p; // power

			j++;
		}

		cls[cl_cnt].first_cpu = next_cpu;
		cls[cl_cnt].last_cpu = last_cpu;
		cl_cnt++;

		next_cpu = last_cpu + 1;
	}

	return 0;
}

// power
static ssize_t show_power(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	int i, j;
	int ret = 0;
	for (i = 0; i < MAX_CLUSTER; i++) {
		ret += sprintf(b + ret, "cluster[%d] = ", i);
		for (j = 0; j < MAX_FREQ; j++)
			ret += sprintf(b + ret, "%u %u\n", cls[i].cpus[j].freq, cls[i].cpus[j].power);
		ret += sprintf(b + ret, "\n");
	}
	b[ret] = '\0';
	return ret;
}
static ssize_t store_power(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	return count;
}
static struct kobj_attribute power_attr = __ATTR(power, 0644, show_power, store_power);

// volt
static ssize_t show_volt(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	int i, j;
	int ret = 0;
	for (i = 0; i < MAX_CLUSTER; i++) {
		ret += sprintf(b + ret, "cluster[%d] = ", i);
		for (j = 0; j < MAX_FREQ; j++)
			ret += sprintf(b + ret, "%u %u\n", cls[i].cpus[j].freq, cls[i].cpus[j].volt);
		ret += sprintf(b + ret, "\n");
	}
	b[ret] = '\0';
	return ret;
}
static ssize_t store_volt(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	return count;
}
static struct kobj_attribute volt_attr = __ATTR(volt, 0644, show_volt, store_volt);

// init
static ssize_t show_init(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	return 0;
}
static ssize_t store_init(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	init_cpu_power();
	return count;
}
static struct kobj_attribute init_attr = __ATTR(init, 0644, show_init, store_init);

// run
static ssize_t show_run(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	return 0;
}
static ssize_t store_run(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	int run = 0;
	if (sscanf(b, "%d", &run) != 1)
		return -EINVAL;
	if (run)
		cpufreq_log_start();
	else
		cpufreq_log_stop();
	return count;
}
static struct kobj_attribute run_attr = __ATTR(run, 0644, show_run, store_run);

static struct kobject *epc_kobj;
static struct attribute *epc_attrs[] = {
	&run_attr.attr,
	&polling_ms_attr.attr,
	&bind_cpu_attr.attr,
	&pid_attr.attr,
	&power_attr.attr,
	&volt_attr.attr,
	&gov_attr.attr,
	&power_limit_attr.attr,
	&init_attr.attr,
	NULL
};
static struct attribute_group epc_group = {
	.attrs = epc_attrs,
};

//================================
// debugfs

static int result_seq_show(struct seq_file *file, void *iter)
{
       if (is_running) {
               seq_printf(file, "NO RESULT\n");
       } else {
               seq_printf(file, "%s", (char *)buf);    // PRINT RESULT
       }
       return 0;
}
static ssize_t result_seq_write(struct file *file, const char __user *buffer, size_t count, loff_t *off)
{
       return count;
}
static int result_debugfs_open(struct inode *inode, struct file *file)
{
       return single_open(file, result_seq_show, inode->i_private);
}
static struct file_operations result_debugfs_fops = {
       .owner          = THIS_MODULE,
       .open           = result_debugfs_open,
       .write          = result_seq_write,
       .read           = seq_read,
       .llseek         = seq_lseek,
       .release        = single_release,
};


/*--------------------------------------*/
// MAIN

int exynos_perf_cpufreq_profile_init(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct dentry *root, *d;
	int ret = 0;

	ret = register_trace_pelt_cfs_tp(update_cpu_util_avg, NULL);
	WARN_ON(ret);

	of_property_read_u32(dn, "cal-id-cpucl0", &cls[0].cal_id);
	of_property_read_u32(dn, "cal-id-cpucl1", &cls[1].cal_id);
	of_property_read_u32(dn, "cal-id-cpucl2", &cls[2].cal_id);

	of_property_read_u32(dn, "cal-id-mif", &cal_id_mif);
	of_property_read_u32(dn, "cal-id-g3d", &cal_id_g3d);
	of_property_read_u32(dn, "devfreq-mif", &devfreq_mif);

	init_cpu_power();

	// sysfs: normal nodes
	epc_kobj = kobject_create_and_add("exynos_perf_cpufreq", kernel_kobj);

	if (!epc_kobj) {
		pr_info("[%s] create node failed: %s\n", prefix, __FILE__);
		return -EINVAL;
	}

	ret = sysfs_create_group(epc_kobj, &epc_group);
	if (ret) {
		pr_info("[%s] create group failed: %s\n", prefix, __FILE__);
		return -EINVAL;
	}

	// debugfs: only for result
	root = debugfs_create_dir("exynos_perf_cpufreq", NULL);
	if (!root) {
		printk("%s: create debugfs\n", __FILE__);
		return -ENOMEM;
	}
	d = debugfs_create_file("result", S_IRUSR, root,
				(unsigned int *)0,
				&result_debugfs_fops);
	if (!d)
		return -ENOMEM;


	return 0;
}
