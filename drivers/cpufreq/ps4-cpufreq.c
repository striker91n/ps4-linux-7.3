// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>

#define MSR_AMD_PSTATE_CUR_LIMIT	0xc0010061
#define MSR_AMD_PERF_CTL		0xc0010062
#define MSR_AMD_PERF_STATUS		0xc0010063
#define MSR_AMD_PSTATE_DEF_BASE		0xc0010064
#define PS4_NUM_PSTATES			8

#define PSTATE_EN			BIT_ULL(63)
#define PSTATE_CPUDID_MASK		GENMASK_ULL(8, 6)
#define PSTATE_CPUFID_MASK		GENMASK_ULL(5, 0)

#define PSTATE_CMD_MASK			GENMASK_ULL(2, 0)
#define PSTATE_STATUS_MASK		GENMASK_ULL(2, 0)
#define PSTATE_MAXVAL_MASK		GENMASK_ULL(6, 4)
#define PSTATE_CURLIMIT_MASK		GENMASK_ULL(2, 0)

static struct cpufreq_frequency_table ps4_freq_table[PS4_NUM_PSTATES + 1];

static unsigned int pstate_to_khz(u64 pstate_def)
{
	u32 cpu_fid = pstate_def & PSTATE_CPUFID_MASK;
	u32 cpu_did = (pstate_def & PSTATE_CPUDID_MASK) >> 6;

	return (100000UL * (cpu_fid + 0x10)) >> cpu_did;
}

static int ps4_cpufreq_target_index(struct cpufreq_policy *policy,
				     unsigned int index)
{
	u32 requested = ps4_freq_table[index].driver_data;
	u64 val;
	int i, cpu;

	rdmsrq_on_cpu(policy->cpu, MSR_AMD_PSTATE_CUR_LIMIT, &val);
	if (requested < (val & PSTATE_CURLIMIT_MASK))
		requested = val & PSTATE_CURLIMIT_MASK;

	for_each_cpu(cpu, policy->cpus)
		wrmsrq_on_cpu(cpu, MSR_AMD_PERF_CTL, requested & PSTATE_CMD_MASK);

	for_each_cpu(cpu, policy->cpus) {
		for (i = 0; i < 100; i++) {
			rdmsrq_on_cpu(cpu, MSR_AMD_PERF_STATUS, &val);
			if ((val & PSTATE_STATUS_MASK) == requested)
				break;
			udelay(100);
		}
		if (i == 100) {
			pr_warn("ps4-cpufreq: cpu%d timed out transitioning to P%u\n",
				cpu, requested);
			return -EIO;
		}
	}

	return 0;
}

static unsigned int ps4_cpufreq_get(unsigned int cpu)
{
	u64 val;
	int i;

	rdmsrq_on_cpu(cpu, MSR_AMD_PERF_STATUS, &val);

	for (i = 0; ps4_freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (ps4_freq_table[i].driver_data == (val & PSTATE_STATUS_MASK))
			return ps4_freq_table[i].frequency;
	}
	return 0;
}

static int ps4_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	policy->freq_table = ps4_freq_table;
	policy->cpuinfo.transition_latency = 100 * 1000;
	cpumask_setall(policy->cpus);

	return 0;
}

static struct cpufreq_driver ps4_cpufreq_driver = {
	.name = "ps4-cpufreq",
	.flags = CPUFREQ_CONST_LOOPS,
	.init = ps4_cpufreq_cpu_init,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = ps4_cpufreq_target_index,
	.get = ps4_cpufreq_get,
};

static int __init ps4_cpufreq_build_table(void)
{
	int i, n = 0;
	u64 val;
	unsigned int khz;
	bool duplicate;
	int j;

	for (i = 0; i < PS4_NUM_PSTATES; i++) {
		rdmsrq(MSR_AMD_PSTATE_DEF_BASE + i, val);
		if (!(val & PSTATE_EN))
			continue;

		khz = pstate_to_khz(val);
		duplicate = false;
		for (j = 0; j < n; j++) {
			if (ps4_freq_table[j].frequency == khz) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		ps4_freq_table[n].driver_data = i;
		ps4_freq_table[n].frequency = khz;
		n++;
	}
	ps4_freq_table[n].driver_data = 0;
	ps4_freq_table[n].frequency = CPUFREQ_TABLE_END;

	return n;
}

static int __init ps4_cpufreq_init(void)
{
	int n;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	if (!boot_cpu_has(X86_FEATURE_HW_PSTATE))
		return -ENODEV;

	n = ps4_cpufreq_build_table();
	if (n == 0) {
		pr_err("ps4-cpufreq: no valid P-states found\n");
		return -ENODEV;
	}

	return cpufreq_register_driver(&ps4_cpufreq_driver);
}

device_initcall(ps4_cpufreq_init);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("cpufreq driver for PS4 (AMD Jaguar, Family 16h)");
