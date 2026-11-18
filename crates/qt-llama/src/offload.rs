//! GPU layer offload planning (archived `llama_offload.cpp`).
//!
//! With `gpuLayers` configured, that value is tried first and CPU (0) is the
//! fallback. Otherwise the plan reserves 1 GiB of headroom per device, offloads
//! everything when the model fits in the remaining VRAM (999 layers), else a
//! proportional layer count, then retries at 75 %, 50 % and 25 % before CPU.

use serde_json::{Value, json};

pub const DEVICE_HEADROOM_BYTES: u64 = 1024 * 1024 * 1024;
pub const DEFAULT_BLOCK_COUNT: i64 = 32;
/// Layer count meaning "offload all layers".
pub const ALL_LAYERS: i32 = 999;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GpuMemory {
    pub index: usize,
    pub free_bytes: u64,
    pub total_bytes: u64,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct OffloadPlanInput {
    pub configured_gpu_layers: Option<i32>,
    pub block_count: Option<i64>,
    pub model_size_bytes: u64,
    pub devices: Vec<GpuMemory>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct OffloadPlan {
    pub gpu_layer_attempts: Vec<i32>,
    pub free_bytes: u64,
    pub usable_bytes: u64,
    pub reserved_bytes: u64,
    pub estimated_block_count: i64,
    pub automatic: bool,
    pub block_count_estimated: bool,
}

fn append_unique(attempts: &mut Vec<i32>, value: i32) {
    let value = value.max(0);
    if !attempts.contains(&value) {
        attempts.push(value);
    }
}

fn scaled_layers(layers: i32, scale: f64) -> i32 {
    ((f64::from(layers) * scale).floor() as i32).max(1)
}

/// Build the ordered list of `n_gpu_layers` values to try.
pub fn plan_offload(input: &OffloadPlanInput) -> OffloadPlan {
    let mut plan = OffloadPlan::default();
    for device in &input.devices {
        plan.free_bytes += device.free_bytes;
        let reserved = device.free_bytes.min(DEVICE_HEADROOM_BYTES);
        plan.reserved_bytes += reserved;
        plan.usable_bytes += device.free_bytes - reserved;
    }
    if let Some(configured) = input.configured_gpu_layers {
        append_unique(&mut plan.gpu_layer_attempts, configured);
        if configured > 0 {
            append_unique(&mut plan.gpu_layer_attempts, 0);
        }
        return plan;
    }

    plan.automatic = true;
    if plan.usable_bytes == 0 || input.model_size_bytes == 0 {
        plan.gpu_layer_attempts.push(0);
        return plan;
    }

    let configured_blocks = input.block_count.unwrap_or(0);
    plan.block_count_estimated = configured_blocks <= 0;
    plan.estimated_block_count = if configured_blocks > 0 {
        configured_blocks
    } else {
        DEFAULT_BLOCK_COUNT
    };
    let total_layers = (plan.estimated_block_count + 1).min(i64::from(i32::MAX)) as i32;

    let mut initial = ALL_LAYERS;
    if plan.usable_bytes < input.model_size_bytes {
        let fraction = plan.usable_bytes as f64 / input.model_size_bytes as f64;
        initial = ((fraction * f64::from(total_layers)).floor() as i32).max(1);
    }
    append_unique(&mut plan.gpu_layer_attempts, initial);
    let base = if initial == ALL_LAYERS {
        total_layers
    } else {
        initial
    };
    append_unique(&mut plan.gpu_layer_attempts, scaled_layers(base, 0.75));
    append_unique(&mut plan.gpu_layer_attempts, scaled_layers(base, 0.5));
    append_unique(&mut plan.gpu_layer_attempts, scaled_layers(base, 0.25));
    append_unique(&mut plan.gpu_layer_attempts, 0);
    plan
}

impl OffloadPlan {
    /// Status JSON recorded per instance (`metadata.offload`).
    pub fn status_json(&self, model_bytes: u64) -> Value {
        let mut status = json!({
            "policy": if self.automatic { "automatic" } else { "configured" },
            "modelBytes": model_bytes,
            "gpuFreeBytes": self.free_bytes,
            "gpuUsableBytes": self.usable_bytes,
            "gpuReservedBytes": self.reserved_bytes,
            "attemptedGpuLayers": [],
        });
        if self.estimated_block_count > 0 {
            status["blockCount"] = json!(self.estimated_block_count);
            status["blockCountEstimated"] = json!(self.block_count_estimated);
        }
        status
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GIB: u64 = 1024 * 1024 * 1024;

    fn device(free: u64) -> GpuMemory {
        GpuMemory {
            index: 0,
            free_bytes: free,
            total_bytes: free,
        }
    }

    #[test]
    fn configured_layers_fall_back_to_cpu() {
        let plan = plan_offload(&OffloadPlanInput {
            configured_gpu_layers: Some(20),
            devices: vec![device(8 * GIB)],
            ..Default::default()
        });
        assert_eq!(plan.gpu_layer_attempts, vec![20, 0]);
        assert!(!plan.automatic);
        let cpu = plan_offload(&OffloadPlanInput {
            configured_gpu_layers: Some(0),
            ..Default::default()
        });
        assert_eq!(cpu.gpu_layer_attempts, vec![0]);
        let negative = plan_offload(&OffloadPlanInput {
            configured_gpu_layers: Some(-1),
            ..Default::default()
        });
        assert_eq!(negative.gpu_layer_attempts, vec![0]);
    }

    #[test]
    fn full_offload_when_model_fits() {
        let plan = plan_offload(&OffloadPlanInput {
            block_count: Some(24),
            model_size_bytes: GIB,
            devices: vec![device(4 * GIB)],
            ..Default::default()
        });
        assert_eq!(plan.usable_bytes, 3 * GIB);
        assert_eq!(plan.reserved_bytes, GIB);
        // 25 total layers: 999, 18, 12, 6, 0.
        assert_eq!(plan.gpu_layer_attempts, vec![999, 18, 12, 6, 0]);
        assert!(!plan.block_count_estimated);
    }

    #[test]
    fn partial_offload_scales_by_free_memory() {
        let plan = plan_offload(&OffloadPlanInput {
            model_size_bytes: 8 * GIB,
            devices: vec![
                device(3 * GIB),
                GpuMemory {
                    index: 1,
                    free_bytes: GIB / 2,
                    total_bytes: GIB,
                },
            ],
            ..Default::default()
        });
        // usable = 2 GiB of 8 GiB; estimated 32 blocks → 33 layers → floor(8.25) = 8.
        assert_eq!(plan.usable_bytes, 2 * GIB);
        assert_eq!(plan.reserved_bytes, GIB + GIB / 2);
        assert_eq!(plan.gpu_layer_attempts, vec![8, 6, 4, 2, 0]);
        assert!(plan.block_count_estimated);
        assert_eq!(plan.estimated_block_count, 32);
        let status = plan.status_json(8 * GIB);
        assert_eq!(status["policy"], "automatic");
        assert_eq!(status["blockCountEstimated"], true);
    }

    #[test]
    fn no_gpu_means_cpu_only() {
        let plan = plan_offload(&OffloadPlanInput {
            model_size_bytes: GIB,
            devices: vec![device(GIB / 2)],
            ..Default::default()
        });
        assert_eq!(plan.gpu_layer_attempts, vec![0]);
        assert!(plan.automatic);
        let tiny = plan_offload(&OffloadPlanInput {
            model_size_bytes: 100 * GIB,
            block_count: Some(2),
            devices: vec![device(GIB + 1)],
            ..Default::default()
        });
        assert_eq!(tiny.gpu_layer_attempts, vec![1, 0]);
    }
}
