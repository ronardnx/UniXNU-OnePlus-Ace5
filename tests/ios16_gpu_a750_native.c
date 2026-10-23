/* Host test for the generated Adreno 750 platform data.
 *
 * Every expectation below is either a value from the pinned vendor source or a
 * word the running device actually wrote in the r5 capture
 * (out/ios16/gpu-native-hwsched-terminal/20261023-194032-290592/result.json,
 * configured_registers GEN7_CP_PROTECT_REG+0/1). Encoding a protect span here
 * and matching that capture proves both the table and the encoding formula. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/native/giulia_a750_native.h"

static uint32_t protect_word(const uint32_t row[4])
{
    const uint32_t index = row[0], start = row[1], end = row[2], deny = row[3];
    assert(end >= start);
    assert(start <= 0x3ffffu && end <= 0x3ffffu);
    assert(index == row[0]);
    return start | ((end - start) << 18) | (deny << 31);
}

int main(void)
{
    /* Identity and limits: device tree + adreno_gpu_core_gen7_9_1. */
    assert(GIULIA_A750_CHIP_ID == 0x43051401u);
    assert(GIULIA_A750_GPU_ID == 750u);
    assert(!strcmp(GIULIA_A750_MODEL, "Adreno750v2"));
    assert(!strcmp(GIULIA_A750_COMPATIBLE, "qcom,adreno-gpu-gen7-9-1"));
    assert(GIULIA_A750_GMEM_BYTES == 0x300000u);            /* 3 * SZ_1M */
    assert(GIULIA_A750_UCHE_GMEM_ALIGNMENT == 0x1000000u);  /* SZ_16M */
    assert(GIULIA_A750_UCHE_GMEM_BASE == 0x1000000u);       /* ALIGN(gmem, align) */
    assert(GIULIA_A750_UCHE_GMEM_BASE + GIULIA_A750_GMEM_BYTES == 0x1300000u);
    assert(GIULIA_A750_HIGHEST_BANK_BIT == 16u);
    assert(GIULIA_A750_GMU_HUB_CLK_HZ == 200000000u);
    assert(GIULIA_A750_HANG_DETECT_CYCLES == 0xcfffffu);
    assert(GIULIA_A750_BCL_DATA == 1u);
    assert(GIULIA_A750_PREEMPT_LEVEL == 1u);

    /* SMMU routes: user 0, LPAC 1, secure 2, GMU 5. */
    assert(GIULIA_A750_SID_USER == 0u && GIULIA_A750_SID_LPAC == 1u);
    assert(GIULIA_A750_SID_SECURE == 2u && GIULIA_A750_SID_GMU == 5u);
    assert(GIULIA_A750_SMMU_ACTLR_CELLS == 3u);

    /* GMU frequency table: 260 MHz / 625 MHz from qcom,gmu-freq-table. */
    assert(GIULIA_A750_GMU_FREQ_COUNT == 2u);
    assert(GiuliaA750GmuFrequency[0] == 260000000u);
    assert(GiuliaA750GmuFrequency[2] == 625000000u);

    /* Register word offsets used by the CP path. */
    assert(GIULIA_A750_CP_SQE_INSTR_BASE_LO == 0x830u);
    assert(GIULIA_A750_CP_SQE_INSTR_BASE_HI == 0x831u);
    assert(GIULIA_A750_CP_RB_WPTR == 0x807u);
    assert(GIULIA_A750_CP_SQE_CNTL == 0x808u);
    assert(GIULIA_A750_GPU_GMU_AO_GPU_CX_BUSY_STATUS == 0x23b0cu);
    assert(GIULIA_A750_RBBM_SECVID_TRUST_CNTL == 0xf400u);

    /* Only the AO CGC table exists for gen7_9_1; there is no GX hwcg list, so
     * a driver must take the RBBM_CGC_GLOBAL_LOAD path instead. */
    assert(GIULIA_A750_AO_HWCG_COUNT == 3u);
    assert(GiuliaA750AoHwcg[0][0] == 0x23b09u && GiuliaA750AoHwcg[0][1] == 0x20202u);
    assert(GiuliaA750AoHwcg[1][0] == 0x23b0au && GiuliaA750AoHwcg[1][1] == 0x10111u);
    assert(GiuliaA750AoHwcg[2][0] == 0x23b0bu && GiuliaA750AoHwcg[2][1] == 0x5555u);

    /* GBIF list is gen7_0_0_gbif_regs: four QSB sides then the client QOS. */
    assert(GIULIA_A750_GBIF_COUNT == 5u);
    for (unsigned i = 0; i < 4; ++i) {
        assert(GiuliaA750Gbif[i][0] == 0x3c03u + i);
        assert(GiuliaA750Gbif[i][1] == 0x71620u);
    }
    assert(GiuliaA750Gbif[4][0] == 0x11u && GiuliaA750Gbif[4][1] == 0x2120212u);

    /* CP protection: 48 contiguous spans, encoded as the vendor writes them. */
    assert(GIULIA_A750_PROTECT_COUNT == 48u);
    for (unsigned i = 0; i < GIULIA_A750_PROTECT_COUNT; ++i) {
        assert(GiuliaA750Protect[i][0] == i);
        assert(GiuliaA750Protect[i][3] <= 1u);
    }
    /* Two words actually programmed on hardware during r5. */
    assert(protect_word(GiuliaA750Protect[0]) == 0x13fc0000u);
    assert(protect_word(GiuliaA750Protect[1]) == 0x0160050bu);
    /* Deny bit comes from the vendor table, not from a default. */
    assert((protect_word(GiuliaA750Protect[6]) >> 31) == 1u);
    assert((protect_word(GiuliaA750Protect[8]) >> 31) == 0u);

    /* Preemption power-up list: gen7_pwrup_reglist, 17 registers. */
    assert(GIULIA_A750_PWRUP_COUNT == 17u);
    assert(GiuliaA750PwrupRegs[0] == 0xe09u);
    assert(GiuliaA750PwrupRegs[1] == 0xe0au);
    assert(GiuliaA750PwrupRegs[16] == 0xb602u);
    for (unsigned i = 1; i < GIULIA_A750_PWRUP_COUNT; ++i)
        assert(GiuliaA750PwrupRegs[i] != GiuliaA750PwrupRegs[i - 1] || i == 0);

    /* Firmware identity: names and byte counts from vendor.img. */
    assert(!strcmp(GIULIA_A750_FW_GMU_NAME, "gmu_gen70900.bin"));
    assert(GIULIA_A750_FW_GMU_BYTES == 78588u);
    assert(!strcmp(GIULIA_A750_FW_SQE_NAME, "gen70900_sqe.fw"));
    assert(GIULIA_A750_FW_SQE_BYTES == 85364u);
    assert(!strcmp(GIULIA_A750_FW_AQE_NAME, "gen70900_aqe.fw"));
    assert(GIULIA_A750_FW_AQE_BYTES == 33176u);
    assert(!strcmp(GIULIA_A750_FW_ZAP_NAME, "gen70900_zap.mbn"));
    assert(GIULIA_A750_FW_ZAP_BYTES == 12088u);
    assert(strlen(GIULIA_A750_FW_SQE_SHA256) == 64u);

    printf("A750: identity, GMEM base/alignment, SMMU routes, 3 AO CGC registers, "
           "5-entry GBIF list, 48 protection spans (two matching the r5 hardware "
           "writes), 17-entry power-up list and firmware identity passed\n");
    return 0;
}
