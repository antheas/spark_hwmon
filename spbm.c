// SPDX-License-Identifier: GPL-2.0
/*
 * NVIDIA DGX Spark (GB10) SPBM Power Telemetry hwmon driver
 *
 * Exposes the System Power Budget Manager (SPBM) shared memory as
 * standard Linux hwmon sensors. The MTEL (NVDA8800) ACPI device
 * provides a _DSM that describes its memory resources; this driver
 * queries _DSM function 1 at probe time to locate the "SPBM" region
 * by name, and _DSM function 2 to discover register offsets by their
 * canonical names rather than hard-coding addresses.
 *
 * The SPBM firmware (running on MediaTek SSPM) continuously updates
 * these registers with live power telemetry in milliwatts,
 * cumulative energy counters in millijoules, and thermal zone
 * temperatures in centidegrees Celsius.
 *
 * This driver binds as an acpi_driver to the NVDA8800 device on the
 * ACPI bus. The device has no platform_device (missing _UID/_STA in
 * DSDT), so a platform_driver cannot be used.
 *
 * Usage:
 *   sudo modprobe spbm   (or auto-loaded via DKMS + udev)
 *   sensors spbm-*
 *   cat /sys/class/hwmon/hwmonN/power1_input   # microwatts
 *
 * Discovered by reverse-engineering the DSDT _DSM for NVDA8800.
 * No upstream driver exists as of kernel 7.0.
 */

#include <linux/module.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/acpi.h>
#include <linux/list.h>
#include <linux/uuid.h>

#define DRIVER_NAME	"spbm"
#define SPBM_SIZE	0x1000

/* Sentinel: offset not discovered via _DSM */
#define OFF_UNKNOWN	U32_MAX

/*
 * _DSM UUID for NVDA8800 MTEL device.
 * Function 1 returns resource names, function 2 returns register maps.
 */
static const guid_t mtel_dsm_guid =
	GUID_INIT(0x12345678, 0x1234, 0x1234,
		  0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc);

/*
 * Channel definition: _DSM register name -> hwmon label.
 * Offsets are discovered at probe time via _DSM function 2.
 */
struct spbm_chan {
	const char *dsm_key;	/* _DSM register name to match */
	const char *label;	/* hwmon label */
};

/* Power channels (mW in firmware, uW in hwmon) */
static const struct spbm_chan pwr_chans[] = {
	{ "SPBM_TE_SYS_TOTAL_TELEMETRY_OFFSET",	"sys_total" },
	{ "SPBM_TE_SOC_PKG_TELEMETRY_OFFSET",		"soc_pkg" },
	{ "SPBM_TE_C_AND_G_TELEMETRY_OFFSET",		"cpu_gpu" },
	{ "SPBM_TE_CPU_P_TELEMETRY_OFFSET",		"cpu_p" },
	{ "SPBM_TE_CPU_E_TELEMETRY_OFFSET",		"cpu_e" },
	{ "SPBM_TE_VCORE_TELEMETRY_OFFSET",		"vcore" },
	{ "SPBM_TE_VDDQ_TELEMETRY_OFFSET",		"vddq" },
	{ "SPBM_TE_CHR_TELEMETRY_OFFSET",		"dc_input" },
	{ "SPBM_TE_TOTAL_GPU_OUT_OFFSET",		"gpu" },
	{ "SPBM_TE_PREREG_IN_OFFSET",			"prereg" },
	{ "SPBM_TE_DLA_IN_OFFSET",			"dla" },
	{ "SPBM_PL1_VAL_OFFSET",			"pl1" },
	{ "SPBM_PL2_VAL_OFFSET",			"pl2" },
	{ "SPBM_SYSPL1_VAL_OFFSET",			"syspl1" },
	{ "SPBM_SYSPL2_VAL_OFFSET",			"syspl2" },
	{ "SPBM_BUDGET_CPU_INST_OFFSET",		"budget_cpu" },
	{ "SPBM_BUDGET_GPU_INST_OFFSET",		"budget_gpu" },
	{ "SPBM_BUDGET_CPU_E_INST_OFFSET",		"budget_cpu_e" },
	{ "SPBM_BUDGET_CPU_P_INST_OFFSET",		"budget_cpu_p" },
	/* EWMA-smoothed power averages per PL controller (mW) */
	{ "SPBM_PWR_AVG_EWMA_S_PL1_OFFSET",		"ewma_pl1" },
	{ "SPBM_PWR_AVG_EWMA_S_PL2_OFFSET",		"ewma_pl2" },
	{ "SPBM_PWR_AVG_EWMA_S_SYSPL1_OFFSET",	"ewma_syspl1" },
	{ "SPBM_PWR_AVG_EWMA_S_SYSPL2_OFFSET",	"ewma_syspl2" },
};
#define N_PWR ARRAY_SIZE(pwr_chans)

/* Energy channels (mJ in firmware, uJ in hwmon) */
static const struct spbm_chan nrg_chans[] = {
	{ "SPBM_PKG_ENERGY_VALUE_ACCUMULATE_OFFSET",	"pkg" },
	{ "SPBM_CPU_E_ENERGY_VALUE_ACCUMULATE_OFFSET",	"cpu_e" },
	{ "SPBM_CPU_P_ENERGY_VALUE_ACCUMULATE_OFFSET",	"cpu_p" },
	{ "SPBM_GPM_ENERGY_VALUE_ACCUMULATE_OFFSET",	"gpu" },
};
#define N_NRG ARRAY_SIZE(nrg_chans)

/* Temperature channels (centidegrees C in firmware, millidegrees C in hwmon) */
static const struct spbm_chan temp_chans[] = {
	{ "SPBM_PKG_TJ_MAX_OFFSET",				"tj_max" },
	{ "SPBM_PKG_TJ_MAX_C_OFFSET",				"tj_max_c" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_E_CLU_0_OFFSET",	"cpu_e_clu0" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_P_CLU_0_OFFSET",	"cpu_p_clu0" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_E_CLU_1_OFFSET",	"cpu_e_clu1" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_CPU_P_CLU_1_OFFSET",	"cpu_p_clu1" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_GPU_OFFSET",		"gpu" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_SOC_OFFSET",		"soc" },
	{ "SPBM_PKG_THERMAL_ZONE_TEMP_DLA_OFFSET",		"dla" },
};
#define N_TEMP ARRAY_SIZE(temp_chans)

/* Status registers (dimensionless) exposed as plain sysfs attributes */
static const struct spbm_chan status_chans[] = {
	{ "SPBM_PROCHOT_STATUS_OFFSET",		"prochot" },
	{ "SPBM_PL_CUR_LEVEL_STATUS_OFFSET",		"pl_level" },
};
#define N_STATUS ARRAY_SIZE(status_chans)

struct spbm_priv {
	void __iomem *base;
	u32 pwr_off[N_PWR];
	u32 nrg_off[N_NRG];
	u32 temp_off[N_TEMP];
	u32 status_off[N_STATUS];
};

/* hwmon callbacks */

static umode_t spbm_visible(const void *data, enum hwmon_sensor_types type,
			     u32 attr, int ch)
{
	const struct spbm_priv *p = data;

	if (type == hwmon_power && ch < N_PWR &&
	    p->pwr_off[ch] != OFF_UNKNOWN &&
	    (attr == hwmon_power_input || attr == hwmon_power_label))
		return 0444;
	if (type == hwmon_energy && ch < N_NRG &&
	    p->nrg_off[ch] != OFF_UNKNOWN &&
	    (attr == hwmon_energy_input || attr == hwmon_energy_label))
		return 0444;
	if (type == hwmon_temp && ch < N_TEMP &&
	    p->temp_off[ch] != OFF_UNKNOWN &&
	    (attr == hwmon_temp_input || attr == hwmon_temp_label))
		return 0444;
	return 0;
}

static int spbm_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int ch, long *val)
{
	struct spbm_priv *p = dev_get_drvdata(dev);
	u32 raw;

	if (type == hwmon_power && attr == hwmon_power_input && ch < N_PWR &&
	    p->pwr_off[ch] != OFF_UNKNOWN) {
		raw = ioread32(p->base + p->pwr_off[ch]);
		*val = (long)raw * 1000; /* mW -> uW */
		return 0;
	}
	if (type == hwmon_energy && attr == hwmon_energy_input && ch < N_NRG &&
	    p->nrg_off[ch] != OFF_UNKNOWN) {
		raw = ioread32(p->base + p->nrg_off[ch]);
		*val = (long)raw * 1000; /* mJ -> uJ */
		return 0;
	}
	if (type == hwmon_temp && attr == hwmon_temp_input && ch < N_TEMP &&
	    p->temp_off[ch] != OFF_UNKNOWN) {
		raw = ioread32(p->base + p->temp_off[ch]);
		*val = (long)raw * 10; /* centidegrees -> millidegrees C */
		return 0;
	}
	return -EOPNOTSUPP;
}

static int spbm_read_string(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int ch, const char **str)
{
	if (type == hwmon_power && ch < N_PWR) {
		*str = pwr_chans[ch].label;
		return 0;
	}
	if (type == hwmon_energy && ch < N_NRG) {
		*str = nrg_chans[ch].label;
		return 0;
	}
	if (type == hwmon_temp && ch < N_TEMP) {
		*str = temp_chans[ch].label;
		return 0;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops spbm_ops = {
	.is_visible = spbm_visible,
	.read = spbm_read,
	.read_string = spbm_read_string,
};

/* Build config arrays with a trailing 0 sentinel */

static const u32 pwr_cfg[N_PWR + 1] = {
	[0 ... N_PWR - 1] = HWMON_P_INPUT | HWMON_P_LABEL,
	[N_PWR] = 0,
};

static const u32 nrg_cfg[N_NRG + 1] = {
	[0 ... N_NRG - 1] = HWMON_E_INPUT | HWMON_E_LABEL,
	[N_NRG] = 0,
};

static const u32 temp_cfg[N_TEMP + 1] = {
	[0 ... N_TEMP - 1] = HWMON_T_INPUT | HWMON_T_LABEL,
	[N_TEMP] = 0,
};

static const struct hwmon_channel_info pwr_info = {
	.type = hwmon_power,
	.config = pwr_cfg,
};

static const struct hwmon_channel_info nrg_info = {
	.type = hwmon_energy,
	.config = nrg_cfg,
};

static const struct hwmon_channel_info temp_info = {
	.type = hwmon_temp,
	.config = temp_cfg,
};

static const struct hwmon_channel_info * const spbm_info[] = {
	&pwr_info, &nrg_info, &temp_info, NULL,
};

static const struct hwmon_chip_info spbm_chip = {
	.ops = &spbm_ops,
	.info = spbm_info,
};

/* Custom sysfs attributes for dimensionless status registers */

static ssize_t prochot_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct spbm_priv *p = dev_get_drvdata(dev);

	if (p->status_off[0] == OFF_UNKNOWN)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", ioread32(p->base + p->status_off[0]));
}
static DEVICE_ATTR_RO(prochot);

static ssize_t pl_level_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct spbm_priv *p = dev_get_drvdata(dev);

	if (p->status_off[1] == OFF_UNKNOWN)
		return -ENODATA;
	return sysfs_emit(buf, "%u\n", ioread32(p->base + p->status_off[1]));
}
static DEVICE_ATTR_RO(pl_level);

static struct attribute *spbm_status_attrs[] = {
	&dev_attr_prochot.attr,
	&dev_attr_pl_level.attr,
	NULL,
};

static umode_t spbm_status_visible(struct kobject *kobj, struct attribute *a,
				    int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct spbm_priv *p = dev_get_drvdata(dev);

	if (idx < N_STATUS && p->status_off[idx] != OFF_UNKNOWN)
		return 0444;
	return 0;
}

static const struct attribute_group spbm_status_group = {
	.attrs = spbm_status_attrs,
	.is_visible = spbm_status_visible,
};

static const struct attribute_group *spbm_extra_groups[] = {
	&spbm_status_group,
	NULL,
};

/* ACPI _DSM helpers */

/*
 * Query _DSM function 1 to get the resource name list, then return
 * the index of the entry matching @name.  Returns -ENOENT if not found.
 */
static int spbm_dsm_find_resource(acpi_handle handle, const char *name)
{
	union acpi_object *out, *elem;
	int i, ret = -ENOENT;

	out = acpi_evaluate_dsm(handle, &mtel_dsm_guid, 0, 1, NULL);
	if (!out)
		return -ENODEV;

	if (out->type != ACPI_TYPE_PACKAGE) {
		ret = -EINVAL;
		goto free;
	}

	for (i = 0; i < out->package.count; i++) {
		elem = &out->package.elements[i];
		if (elem->type == ACPI_TYPE_STRING &&
		    !strcmp(elem->string.pointer, name)) {
			ret = i;
			break;
		}
	}

free:
	ACPI_FREE(out);
	return ret;
}

/*
 * Look up a DSM key in a channel table and store the offset.
 * Returns true if the key matched a channel.
 */
static bool spbm_try_resolve(const char *key, u64 offset,
			     const struct spbm_chan *chans, u32 *offsets,
			     int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!strcmp(chans[i].dsm_key, key)) {
			offsets[i] = (u32)offset;
			return true;
		}
	}
	return false;
}

/*
 * Call _DSM function 2 with sub-index @sub_idx.  Walk the returned
 * nested packages of {count, "name", offset, ...} pairs and resolve
 * offsets for all channel tables.
 */
static int spbm_dsm_resolve_offsets(struct device *dev, acpi_handle handle,
				    int sub_idx, struct spbm_priv *p)
{
	union acpi_object arg_elem, arg_pkg;
	union acpi_object *out, *sub, *elem;
	int i, j, count, resolved = 0;

	arg_elem.type = ACPI_TYPE_INTEGER;
	arg_elem.integer.value = sub_idx;
	arg_pkg.type = ACPI_TYPE_PACKAGE;
	arg_pkg.package.count = 1;
	arg_pkg.package.elements = &arg_elem;

	out = acpi_evaluate_dsm(handle, &mtel_dsm_guid, 0, 2, &arg_pkg);
	if (!out)
		return -ENODEV;

	if (out->type != ACPI_TYPE_PACKAGE) {
		ACPI_FREE(out);
		return -EINVAL;
	}

	/* Walk each sub-package: {count, name1, off1, name2, off2, ...} */
	for (i = 0; i < out->package.count; i++) {
		sub = &out->package.elements[i];
		if (sub->type != ACPI_TYPE_PACKAGE || sub->package.count < 3)
			continue;

		elem = sub->package.elements;
		if (elem[0].type != ACPI_TYPE_INTEGER)
			continue;
		count = elem[0].integer.value;

		for (j = 0; j < count; j++) {
			int ni = 1 + j * 2;	/* name index */
			int oi = 2 + j * 2;	/* offset index */

			if (oi >= sub->package.count)
				break;
			if (elem[ni].type != ACPI_TYPE_STRING ||
			    elem[oi].type != ACPI_TYPE_INTEGER)
				continue;

			if (spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     pwr_chans, p->pwr_off, N_PWR) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     nrg_chans, p->nrg_off, N_NRG) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     temp_chans, p->temp_off, N_TEMP) ||
			    spbm_try_resolve(elem[ni].string.pointer,
					     elem[oi].integer.value,
					     status_chans, p->status_off,
					     N_STATUS))
				resolved++;
		}
	}

	ACPI_FREE(out);
	return resolved;
}

/* ACPI driver */

static int spbm_add(struct acpi_device *adev)
{
	struct device *dev = &adev->dev;
	struct list_head res_list;
	struct resource_entry *re;
	resource_size_t phys = 0;
	struct spbm_priv *p;
	struct device *hwdev;
	int spbm_idx, idx = 0, ret, resolved;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	/* Initialize all offsets to "unknown" */
	memset(p->pwr_off, 0xFF, sizeof(p->pwr_off));
	memset(p->nrg_off, 0xFF, sizeof(p->nrg_off));
	memset(p->temp_off, 0xFF, sizeof(p->temp_off));
	memset(p->status_off, 0xFF, sizeof(p->status_off));

	/* Ask _DSM which _CRS resource is "SPBM" */
	spbm_idx = spbm_dsm_find_resource(adev->handle, "SPBM");
	if (spbm_idx < 0) {
		dev_err(dev, "_DSM did not advertise SPBM resource (%d)\n",
			spbm_idx);
		return spbm_idx;
	}

	/* Resolve register offsets from _DSM function 2 */
	resolved = spbm_dsm_resolve_offsets(dev, adev->handle, spbm_idx, p);
	if (resolved < 0) {
		dev_err(dev, "_DSM register map query failed (%d)\n", resolved);
		return resolved;
	}
	dev_info(dev, "resolved %d/%zu register offsets from _DSM\n",
		 resolved, N_PWR + N_NRG + N_TEMP + N_STATUS);

	/* Walk _CRS to find the memory resource at that index */
	INIT_LIST_HEAD(&res_list);
	ret = acpi_dev_get_resources(adev, &res_list, NULL, NULL);
	if (ret < 0)
		return ret;

	list_for_each_entry(re, &res_list, node) {
		if (resource_type(re->res) == IORESOURCE_MEM) {
			if (idx == spbm_idx) {
				phys = re->res->start;
				break;
			}
			idx++;
		}
	}
	acpi_dev_free_resource_list(&res_list);

	if (!phys) {
		dev_err(dev, "SPBM memory resource (index %d) not in _CRS\n",
			spbm_idx);
		return -ENODEV;
	}

	p->base = devm_ioremap(dev, phys, SPBM_SIZE);
	if (!p->base)
		return -ENOMEM;

	/* Sanity check: read first power channel if resolved */
	if (p->pwr_off[0] != OFF_UNKNOWN) {
		u32 test = ioread32(p->base + p->pwr_off[0]);

		if (test == 0 || test == 0xFFFFFFFF)
			dev_warn(dev, "%s=%u, telemetry may be inactive\n",
				 pwr_chans[0].label, test);
		else
			dev_info(dev, "live at 0x%llx (res %d): %s=%u mW\n",
				 (u64)phys, spbm_idx,
				 pwr_chans[0].label, test);
	}

	hwdev = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, p,
						     &spbm_chip,
						     spbm_extra_groups);
	if (IS_ERR(hwdev))
		return PTR_ERR(hwdev);

	dev_info(dev, "registered %zu power + %zu energy + %zu temp channels\n",
		 N_PWR, N_NRG, N_TEMP);

	return 0;
}

static const struct acpi_device_id spbm_acpi_ids[] = {
	{ "NVDA8800", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, spbm_acpi_ids);

static struct acpi_driver spbm_driver = {
	.name = DRIVER_NAME,
	.ids = spbm_acpi_ids,
	.ops = {
		.add = spbm_add,
	},
};
module_acpi_driver(spbm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DGX Spark Power Telemetry");
MODULE_DESCRIPTION("NVIDIA DGX Spark (GB10) SPBM power hwmon driver");
