/*
 * peridot: Xiaomi POCO F6 / Redmi Turbo 3, Qualcomm SM8635 (Snapdragon 8s Gen 3).
 * Values from the phone's device tree (vendor_boot) and the peridot Mu-Silicium port.
 */
static const struct platform platform_peridot = {
	.name = "peridot", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17100000, .gic_redist = 0x17180000, .gic_redist_size = 0x200000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/* Cores 0-2 are Cortex-A520s, 3-6 Cortex-A720s, 7 a Cortex-X4. */
	.small_cores = 0x07,
	.fb_base = 0xE3940000,
};
