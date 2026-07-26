#!/system/bin/sh
# On-DEVICE orchestration of the SDS reliable-channel repair proof (#211).
# Runs three separate processes (one nwaku node each) on a timeline:
#   send  : publishes M0-live at t=20s, M1-while-offline at t=45s, stays up.
#   recv1 : online for the baseline; KILLED at t=35s (before M1) => "offline".
#   recv2 : a FRESH receiver started at t=40s, after M1 was already published.
# SDS keeps retransmitting the unacked M1 (5x/25s), so recv2 backfills it.
#
#   channels (default): recv2 should print `## GOT M1-while-offline` (REPAIR)
#   LOGOS_DELIVERY_PLAIN=1: recv2 gets nothing (plain relay/filter = no backfill)
#
# Usage (from the device, in the dir holding channel_repair + the .so's):
#   sh channel-repair-proof.sh <topic> [plain]
set -u
cd "$(dirname "$0")"
export LD_LIBRARY_PATH=.
TOPIC="${1:?usage: channel-repair-proof.sh <topic> [plain]}"
# Channels are opt-in (LOGOS_DELIVERY_CHANNELS=1); plain is the default.
if [ "${2:-}" = plain ]; then MODE=plain; else export LOGOS_DELIVERY_CHANNELS=1; MODE=channels; fi

rm -f send.out recv1.out recv2.out
echo "=== repair proof: transport=$MODE topic=$TOPIC ==="

./channel_repair send  "$TOPIC" 60110 20 45 > send.out  2>/dev/null &
SEND=$!
./channel_repair recv  "$TOPIC" 60120        > recv1.out 2>/dev/null &
R1=$!

sleep 35            # recv1 online across M0 (t=20)
kill "$R1" 2>/dev/null   # recv1 OFFLINE at t=35, before M1 at t=45
echo "-- recv1 killed (offline) at t=35 --"

sleep 5             # t=40: bring a fresh receiver up (M1 not yet sent: t=45)
./channel_repair recv  "$TOPIC" 60121        > recv2.out 2>/dev/null &
R2=$!

sleep 45            # t=85: M1 (t=45) + retransmits (t=45..70) land while recv2 up
kill "$R2" "$SEND" 2>/dev/null
sleep 1

echo "===== SEND ====="  ; grep '##' send.out  || echo "(none)"
echo "===== RECV1 (online for baseline) =====" ; grep '##' recv1.out || echo "(none)"
echo "===== RECV2 (returned after offline) ====="; grep '##' recv2.out || echo "(none)"

if grep -q '## GOT M1-while-offline' recv2.out; then
  echo "VERDICT: BACKFILL RECEIVED (SDS repair worked)"
else
  echo "VERDICT: M1 NOT backfilled (lost)"
fi
