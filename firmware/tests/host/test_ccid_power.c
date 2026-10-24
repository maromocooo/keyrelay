/* yd_ccid.h: the PowerOff / PowerOn the desk unit sends itself to reset the card when the receiver
 * reconnects, and how it reads the key's answers. A reset only counts when the key says it did it. */
#include <stdio.h>
#include "yd_ccid.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void rsp(uint8_t out[YD_CCID_HDR], uint8_t type, uint8_t seq, uint8_t status, uint8_t error)
{
    memset(out, 0, YD_CCID_HDR);
    out[0] = type;
    out[6] = seq;
    out[7] = status;
    out[8] = error;
}

int main(void)
{
    uint8_t off[YD_CCID_HDR], on[YD_CCID_HDR], r[YD_CCID_HDR + 20];

    memset(off, 0xAA, sizeof(off));
    CHECK(yd_ccid_make_power_req(CCID_PC_POWER_OFF, 7, off) == YD_CCID_HDR, "a power request is a bare header");
    CHECK(off[0] == CCID_PC_POWER_OFF && off[6] == 7 && yd_ccid_valid(off, YD_CCID_HDR), "PowerOff: type, seq, no data");
    CHECK(off[5] == 0 && off[7] == 0 && off[8] == 0 && off[9] == 0, "PowerOff: slot 0, no leftover bytes");
    yd_ccid_make_power_req(CCID_PC_POWER_ON, 8, on);
    CHECK(on[0] == CCID_PC_POWER_ON && on[6] == 8 && on[7] == 0, "PowerOn: automatic voltage selection");

    rsp(r, CCID_RDR_SLOT_STATUS, 7, CCID_CMD_OK | CCID_ICC_INACTIVE, 0);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_OK, "PowerOff answered with the card inactive");
    rsp(r, CCID_RDR_SLOT_STATUS, 7, CCID_CMD_OK | CCID_ICC_ACTIVE, 0);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "PowerOff that left the card active is a failure");
    rsp(r, CCID_RDR_SLOT_STATUS, 7, CCID_CMD_OK | CCID_ICC_ABSENT, 0);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_OK,
          "PowerOff answered with 'not present' (what a YubiKey 5 says after powering off) is a success");
    rsp(r, CCID_RDR_SLOT_STATUS, 7, CCID_CMD_FAILED | CCID_ICC_ABSENT, CCID_ERR_ICC_MUTE);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "a failed PowerOff is a failure even with the card gone");
    rsp(r, CCID_RDR_DATA_BLOCK, 8, CCID_CMD_OK | CCID_ICC_ABSENT, 0);
    CHECK(yd_ccid_power_resp(on, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "PowerOn that leaves no card is a failure");
    rsp(r, CCID_RDR_SLOT_STATUS, 7, CCID_CMD_FAILED | CCID_ICC_ACTIVE, CCID_ERR_SLOT_BUSY);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "slot busy (an earlier request still running) is a failure");
    rsp(r, CCID_RDR_DATA_BLOCK, 7, CCID_CMD_OK | CCID_ICC_INACTIVE, 0);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "the wrong response type for the seq is a failure");

    rsp(r, CCID_RDR_DATA_BLOCK, 6, CCID_CMD_OK | CCID_ICC_INACTIVE, 0);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_WAIT, "a late answer to another seq is skipped");
    rsp(r, CCID_RDR_DATA_BLOCK, 7, CCID_CMD_TIME_EXT | CCID_ICC_ACTIVE, 1);
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR) == YD_CCID_POWER_WAIT, "a time extension is not the final answer");
    CHECK(yd_ccid_power_resp(off, r, YD_CCID_HDR - 1) == YD_CCID_POWER_WAIT, "a short message is skipped");

    rsp(r, CCID_RDR_DATA_BLOCK, 8, CCID_CMD_OK | CCID_ICC_ACTIVE, 0);
    r[1] = 20;                                   /* the ATR */
    CHECK(yd_ccid_power_resp(on, r, YD_CCID_HDR + 20) == YD_CCID_POWER_OK, "PowerOn answered with an ATR and the card active");
    rsp(r, CCID_RDR_DATA_BLOCK, 8, CCID_CMD_FAILED | CCID_ICC_INACTIVE, CCID_ERR_ICC_MUTE);
    CHECK(yd_ccid_power_resp(on, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "PowerOn that failed is a failure");
    rsp(r, CCID_RDR_SLOT_STATUS, 8, CCID_CMD_OK | CCID_ICC_ACTIVE, 0);
    CHECK(yd_ccid_power_resp(on, r, YD_CCID_HDR) == YD_CCID_POWER_FAIL, "PowerOn answered with a slot status is a failure");

    printf("ccid_power: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
