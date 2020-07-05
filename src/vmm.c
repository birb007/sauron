#include "vmm.h"

#include <asm-generic/errno.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/refcount.h>
#include <linux/smp.h>
#include <linux/types.h>
/*#include <linux/vmalloc.h>*/
#include <asm/io.h>
#include <linux/slab.h>

#include "arch.h"
#include "msr.h"
#include "utils.h"
#include "vmx.h"

static vmm_cpu_enable_vmx(struct vmm_global_ctx* vmm_ctx) {
    const int processor_id = smp_processor_id();
    struct vmm_cpu_ctx ctx = vmm_ctx->each_cpu_ctx[processor_id];

    ctx->processor_id = processor_id;
}

static bool is_system_hyperviseable(void) {
    union ia32_feature_control_msr feature_control_msr;

    /* Does the processor have implemented virtual machine extensions? */
    if (!arch_cpu_has_vmx()) {
        utils_log(err, "CPU does not support VMX\n");
        return false;
    }

    feature_control_msr.value = __rdmsr(IA32_FEATURE_CONTROL_MSR);

    /* BIOS can disable VMX which causes VMXON to generate a #GP fault. The MSR
     * cannot be modified until a power-up reset condition. */
    if (!feature_control_msr.fields.lock) {
        utils_log(err, "BIOS has disabled support for VMX\n");
        return false;
    }

    /* BIOS can disable VMX outside of SMX which causes VMXON to generate a #GP
     * fault. */
    if (!feature_control_msr.fields.enable_vmx_outside_smx) {
        utils_log(err, "BIOS has disabled VMX support outside SMX\n");
        return false;
    }
    return true;
}

static void cpu_ctx_destroy(struct vmm_cpu_ctx* ctx) {
    vmx_vmxon_region_destroy(ctx->vmxon_region);
    /*vmcs_vmcs_region_destroy(ctx->vmcs_region);*/
}

static ssize_t cpu_ctx_init(struct vmm_cpu_ctx* cpu_ctx,
                            struct vmm_global_ctx* vmm_ctx) {
    if (!(cpu_ctx->vmxon_region = vmx_vmxon_region_create(vmm_ctx))) {
        utils_log(err, "unable to create VMXON region\n");
        return -ENOMEM;
    }

    /*
     *if (!(cpu_ctx->vmcs_region = vmcs_vmcs_region_create(vmm_ctx))) {
     *    utils_log(err, "unable to create VMCS region\n");
     *    vmx_vmxon_region_destroy(cpu_ctx->vmxon_region);
     *    return -ENOMEM;
     *}
     */

    /*cpu_ctx->vmcs_region_ptr = virt_to_phys(cpu_ctx->vmcs_region);*/
    cpu_ctx->vmxon_region_ptr = virt_to_phys(cpu_ctx->vmxon_region);
    return 0;
}

void vmm_global_ctx_destroy(struct vmm_global_ctx* ctx) {
    size_t i;
    for (i = 0; i < ctx->n_init_cpus; ++i) {
        cpu_ctx_destroy(&ctx->each_cpu_ctx[i]);
    }

    kfree(ctx->each_cpu_ctx);
    kfree(ctx);
}

static struct vmm_global_ctx* vmm_global_ctx_create(void) {
    size_t i;
    struct vmm_global_ctx* ctx;

    if (!(ctx = kzalloc(sizeof(*ctx), GFP_KERNEL))) {
        utils_log(err, "unable to allocate memory for global VMM context\n");
        return NULL;
    }

    ctx->n_online_cpus = (size_t)num_online_cpus();

    if (!(ctx->each_cpu_ctx = kzalloc(
              ctx->n_online_cpus * sizeof(*ctx->each_cpu_ctx), GFP_KERNEL))) {
        utils_log(err, "unable to allocate memory for processor contexts\n");
        kfree(ctx);
        return NULL;
    }

    for (i = 0; i < ctx->n_online_cpus; ++i) {
        utils_log(info, "processor [%zu]: creating processor context\n", i);

        if (cpu_ctx_init(&ctx->each_cpu_ctx[i], ctx) < 0) {
            utils_log(err,
                      "processor [%zu]: failed to create processor context\n",
                      i);
            vmm_global_ctx_destroy(ctx);
            return NULL;
        }

        ctx->n_init_cpus++;
    }

    return ctx;
}

ssize_t vmm_init_processors(void) {
    struct vmm_global_ctx* ctx;

    /* Determine if the processor is available to enter VMX root operation. */
    if (!is_system_hyperviseable()) {
        return -ENOSYS;
    }
    utils_log(info,
              "detected all required features for hypervisor operation\n");

    if ((ctx = vmm_global_ctx_create()) < 0) {
        utils_log(err, "failed to create global VMM context");
        return -1;
    }

    on_each_cpu(vmm_cpu_enable_vmx, ctx, 1);
    on_each_cpu(vmm_cpu_disable_vmx, ctx, 1);

    vmm_global_ctx_destroy(ctx);
    return 0;
}

ssize_t vmm_exit_root_all_processors(void) { return 0; }
