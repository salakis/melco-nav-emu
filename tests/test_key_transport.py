# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Compile the board model's keypad transaction functions with a minimal UART fixture."""
import re
import subprocess
from pathlib import Path


def test_key_transactions_keep_release_and_wait_for_frame_ack(tmp_path):
    machine = Path(__file__).parents[1] / 'machine'
    source = '\n'.join(p.read_text() for p in [machine / 'cup2006.c'] +
                         sorted(machine.glob('*.c.inc')))
    names = ['cup2006_key_put_frame', 'cup2006_key_guest_tx',
             'cup2006_key_start', 'cup2006_key_pump', 'cup2006_key_enqueue']
    bodies = []
    for name in names:
        match = re.search(r'static void ' + name + r'\([^;]+?\)\n\{', source)
        assert match, name
        depth, end = 1, match.end()
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        bodies.append(source[match.start():end])
    fields = set(re.findall(r's->(\w+)', '\n'.join(bodies)))
    arrays = {'key_q': 'int key_q[16];', 'key_q_raw': 'bool key_q_raw[16];'}
    declarations = '\n'.join(arrays.get(field, f'int {field};') for field in sorted(fields))
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CUP2006_WHEEL_FLAG 0x10000
#define CUP2006_STATUS_FLAG 0x20000
#define CUP2006_SUBPEER_GRANT_MS 50
#define QEMU_CLOCK_VIRTUAL 0
#define cup2006_info(...) ((void)0)
#define cup2006_dbg(...) ((void)0)
/* the board logs through trace events now; stub the ones these bodies call */
#define trace_cup2006_subcpu_keys_guest_acked_key(...) ((void)0)
#define trace_cup2006_subcpu_keys_peer_enq_x05(...) ((void)0)
#define trace_cup2006_subcpu_keys_guest_key_raw(...) ((void)0)
#define trace_cup2006_subcpu_keys_peer_enq_sent(...) ((void)0)
#define trace_cup2006_subcpu_keys_queue_full_dropped(...) ((void)0)
#define trace_cup2006_subcpu_subpeer_status_frame_data(...) ((void)0)
#define PRId64 "lld"
typedef struct { DECLARATIONS } Cup2006MachineState;
static int offers, frames, last_key, scheduled, last_status;
static int64_t qemu_clock_get_ms(int clock) { return 100; }
static void timer_mod(int timer, int64_t when) { scheduled++; }
static int cup2006_scif0_rx_count(Cup2006MachineState *s) { return 0; }
static void cup2006_scif0_peer_send(Cup2006MachineState *s, const uint8_t *p, unsigned n) {
    offers++; s->peer_q_tail += n;
}
static void cup2006_key_put_frame_enc(Cup2006MachineState *s, uint16_t key,
                                    uint8_t type, uint8_t val, bool raw) {
    frames++; last_key=key; s->key_frames++; s->peer_q_tail+=8;
}
static void cup2006_wheel_put_frame(Cup2006MachineState *s, int16_t p) {
    s->key_frames++; s->key_last_wheel=true; s->peer_q_tail+=17;
}
static void cup2006_status_put_frame(Cup2006MachineState *s, unsigned code) {
    last_status=code; s->key_frames++; s->key_last_wheel=false; s->peer_q_tail+=8;
}
static void cup2006_key_put_frame(Cup2006MachineState *s);
static void cup2006_key_start(Cup2006MachineState *s);
static void cup2006_key_enqueue(Cup2006MachineState *s, int code, bool raw);
FUNCTIONS
static void drain(Cup2006MachineState *s) { s->peer_q_head=s->peer_q_tail; }
int main(void) {
    Cup2006MachineState s={0}; s.key_waiting=-1; s.key_seq=1; s.keys=true;
    cup2006_key_enqueue(&s,0x57,true);
    assert(offers==1 && s.peer_enq_armed && s.key_waiting==0x57);
    cup2006_key_enqueue(&s,0,true);
    assert(s.key_q_n==1 && offers==1);
    drain(&s); cup2006_key_guest_tx(&s,6);
    assert(frames==1 && last_key==0x57 && s.key_frame_inflight);
    assert(s.key_waiting==0x57);
    cup2006_key_pump(&s);
    assert(offers==1);  /* pending release cannot interleave an ENQ */
    drain(&s); cup2006_key_guest_tx(&s,6);
    assert(!s.key_frame_inflight && s.key_waiting==-1);
    cup2006_key_pump(&s);
    assert(offers==2 && s.key_waiting==0 && s.peer_enq_armed);
    drain(&s); cup2006_key_guest_tx(&s,0x15);
    assert(!s.peer_enq_armed && s.key_waiting==0 && scheduled);
    cup2006_key_pump(&s);
    assert(offers==3 && s.peer_enq_armed); /* retry retains release */
    drain(&s); cup2006_key_guest_tx(&s,6);
    assert(last_key==0 && s.key_frame_inflight);
    drain(&s); cup2006_key_guest_tx(&s,0x15);
    assert(!s.key_frame_inflight && s.key_waiting==0);
    cup2006_key_pump(&s);
    assert(offers==4 && s.peer_enq_armed);
    /* A busy UART does not consume a queued key or start another offer. */
    Cup2006MachineState busy={0}; busy.key_waiting=-1; busy.peer_q_tail=1;
    cup2006_key_enqueue(&busy,0x53,true);
    assert(busy.key_q_n==1 && busy.key_waiting==-1 && offers==4);
    /* Fill the queue behind an in-flight frame; release has a reserved slot. */
    Cup2006MachineState full={0}; full.key_waiting=0x53; full.key_frame_inflight=true;
    for(int n=0;n<30;n++) cup2006_key_enqueue(&full,0x53,true);
    assert(full.key_q_n==15);
    cup2006_key_enqueue(&full,0,true);
    assert(full.key_q_n==16 && full.key_q[15]==0 && full.key_q_raw[15]);
    cup2006_key_enqueue(&full,0,true);
    assert(full.key_q_n==16 && full.key_q[15]==0);
    /* After we ACK a guest ENQ the guest holds the link: no peer ENQ until its
     * frame is in or the grant lapses. */
    Cup2006MachineState g={0}; g.key_waiting=-1; g.key_seq=1; g.keys=true;
    g.subpeer_grant_ms=90;              /* granted 10 ms ago; clock reads 100 */
    int before=offers, sched=scheduled;
    cup2006_key_enqueue(&g,0x59,true);
    assert(offers==before && g.key_waiting==-1 && g.key_q_n==1 && scheduled>sched);
    g.subpeer_grant_ms=0; g.subpeer_frame_len=3;   /* its frame is arriving */
    cup2006_key_pump(&g);
    assert(offers==before && g.key_waiting==-1);
    g.subpeer_frame_len=0;              /* frame in: the receiver reset cleared the grant */
    cup2006_key_pump(&g);
    assert(offers==before+1 && g.peer_enq_armed && g.key_waiting==0x59);
    /* An idle poll sends no frame; its grant lapses after 50 ms. */
    Cup2006MachineState l={0}; l.key_waiting=-1; l.key_seq=1; l.keys=true;
    l.subpeer_grant_ms=40;
    cup2006_key_enqueue(&l,0x59,true);
    assert(offers==before+2 && l.peer_enq_armed);
    /* A guest ENQ is never answered with a peer ENQ: a latched or offered key
     * withdraws and waits for the frame ACK. */
    sched=scheduled; before=offers;
    l.peer_q_head=l.peer_q_tail; l.key_q_timer=1;
    cup2006_key_guest_tx(&l,5);
    assert(!l.peer_enq_armed && l.key_waiting==0x59 && offers==before && scheduled>sched);
    cup2006_key_guest_tx(&l,5);
    assert(!l.peer_enq_armed && offers==before);
    /* Sub-CPU status frames ride the same exchange; the guest's ACK of NET
     * OFF queues DeviceMode Shutdown and does not count as a key ACK. */
    Cup2006MachineState a={0}; a.key_waiting=-1; a.key_seq=1; a.keys=true;
    cup2006_key_enqueue(&a,CUP2006_STATUS_FLAG|0xf18a,false);
    assert(a.peer_enq_armed);
    drain(&a); cup2006_key_guest_tx(&a,6);
    assert(last_status==0xf18a && a.key_frame_inflight);
    drain(&a); cup2006_key_guest_tx(&a,6);
    assert(a.subpeer_acc_off_stage==2 && a.key_guest_acks==0);
    assert(a.peer_enq_armed && a.key_waiting==(CUP2006_STATUS_FLAG|0xfc81));
    return 0;
}
'''.replace('DECLARATIONS', declarations).replace('FUNCTIONS', '\n'.join(bodies))
    path = tmp_path / 'key_transport.c'
    path.write_text(fixture)
    binary = tmp_path / 'key_transport'
    subprocess.run(['cc', '-std=c11', str(path), '-o', str(binary)], check=True,
                   capture_output=True, text=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_car_voice_grant_frame_passes_sub1_parser(tmp_path):
    """The car's f3 81 answer (voice channel, cup2006-subcpu.c.inc) is a
    well-formed Sub-CPU frame: `02 LEN f3 81 seq kind 01 03 CS`, with a
    correct LEN-seeded checksum through the ETX."""
    machine = Path(__file__).parents[1] / 'machine'
    source = (machine / 'cup2006-subcpu.c.inc').read_text()
    match = re.search(r'static void cup2006_status_put_frame\([^;]+?\)\n\{', source)
    assert match
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    body = source[match.start():end]
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define QEMU_CLOCK_VIRTUAL 0
#define PRId64 "lld"
#define cup2006_info(...) ((void)0)
#define cup2006_dbg(...) ((void)0)
/* the board logs through trace events now; stub the ones these bodies call */
#define trace_cup2006_subcpu_keys_guest_acked_key(...) ((void)0)
#define trace_cup2006_subcpu_keys_peer_enq_x05(...) ((void)0)
#define trace_cup2006_subcpu_keys_guest_key_raw(...) ((void)0)
#define trace_cup2006_subcpu_keys_peer_enq_sent(...) ((void)0)
#define trace_cup2006_subcpu_keys_queue_full_dropped(...) ((void)0)
#define trace_cup2006_subcpu_subpeer_status_frame_data(...) ((void)0)
typedef struct { int car_market; uint8_t car_voice_reply[2]; int key_frames;
                 bool key_last_wheel; } Cup2006MachineState;
static uint8_t out[32]; static unsigned out_n;
static long long qemu_clock_get_ms(int c) { return 0; }
static void cup2006_scif0_peer_send(Cup2006MachineState *s, const uint8_t *p, unsigned n) {
    memcpy(out, p, n); out_n = n;
}
FUNCTION
int main(void) {
    Cup2006MachineState s = { .car_market = 0xa0, .car_voice_reply = { 1, 1 },
                              .key_frames = 4 };
    uint8_t sum;
    unsigned i;
    cup2006_status_put_frame(&s, 0xf381);
    assert(out_n == 9);
    assert(out[0] == 0x02 && out[1] == 7 && out[2] == 0xf3 && out[3] == 0x81);
    assert(out[4] == 5 && out[5] == 1 && out[6] == 1 && out[7] == 0x03);
    sum = out[1];
    for (i = 2; i < out_n - 1; i++) sum += out[i];
    assert(out[8] == sum);
    return 0;
}
'''.replace('FUNCTION', body)
    path = tmp_path / 'voice_grant.c'
    path.write_text(fixture)
    binary = tmp_path / 'voice_grant'
    subprocess.run(['cc', '-std=c11', str(path), '-o', str(binary)], check=True,
                   capture_output=True, text=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
