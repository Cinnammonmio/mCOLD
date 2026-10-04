---
name: User-Manual_EN
lang: en
version: 0.6
status: draft
date: 2026-10-05
firmware: 0.7.0-dev.8
---

# mCOLD Foam V.1 — User Manual


## 1. About mCOLD

mCOLD is a cold-chain box that records the temperature of its goods for
the whole journey. From the start of a trip to its end it:

- measures the temperature with its probe and records it every 5 minutes
- raises an alarm when the temperature leaves the range set for that trip
- keeps the records on board, and sends them to the server over Wi-Fi when it can
- records its position (GNSS) and when the box is moved or knocked

## 2. Parts

| Part | What it does |
|---|---|
| E-paper screen | Shows temperature and status; keeps its picture with no power |
| Three front lights | Left = goods, middle = trip, right = the box itself (see 6) |
| Side light | Charging status; lit only while plugged in |
| Buzzer | Sounds on a temperature alarm |
| NFC tap point | Tap a phone here to connect the app |
| USB-C port | Charging |
| Power switch | Cuts power to the whole box |
| Temperature probe | Measures the goods' temperature |

## 3. Switching on and off

**On:** slide the switch. The three front lights sweep blue, violet and
cyan from left to right, then blink cyan together: the box is ready. Three amber blinks on
the right light instead mean a part inside is not working properly.

**Off:** slide the switch back. Everything stops; the clock keeps running
on its backup cell. If you switch off during a trip, the trip carries on
when the box is switched on again, and the gap is recorded.

**On battery** the box sleeps almost all the time to save power and wakes
itself every 5 minutes to measure and record. The screen keeps showing the
latest picture.

## 4. Battery and charging

Plug in USB-C to charge. The side light shows:

| Side light | Meaning |
|---|---|
| Green, breathing | Charging |
| Green, steady | Full |
| Violet, slow blink | Power in, but not charging |
| Red, blinking | Charging fault (one beep) |
| Off | Not plugged in |

While plugged in, the box stays awake and keeps its Wi-Fi connection.

### Battery protection

| Battery | What the box does |
|---|---|
| Below 15% | Right light blinks amber; a low-battery alarm is recorded in the trip |
| Below about 10% | No new trip can start (the box keeps running) — charge first |
| Below about 5% | Screen shows **BATTERY EMPTY** and the box switches itself off, so the battery is not damaged or swollen |

When the box has switched off for an empty battery, all its data is kept.
Plug in USB-C: the box switches itself back on and any running trip
carries on.

## 5. The screen

| Where | Shows |
|---|---|
| Top bar | Serial number and the local time and date, e.g. 22:50 04/10/26 |
| Centre | Latest temperature (°C), large |
| Below it | Min / max of this trip, or "NO ACTIVE TRIP" |
| Bottom left | Wi-Fi, server, GNSS and trip icons |
| Bottom right | Charging, battery %, storage used % |

A Wi-Fi, server or GNSS icon with a line through it is not connected, or
has no recent data.

**Red on the screen:** red number = temperature out of range · red number
and red frame = alarm · red `--` = no reading from the probe.

**Updates:** the screen changes when its content changes, at most every
5 minutes, but at once for an alarm, a trip starting or stopping, or the
cable going in or out. A refresh takes about 2 seconds.

## 6. Lights and sound

**Red and amber are always warnings, always in the same place:** red on
the left light is the goods, amber on the right is the box. Blue, violet
and cyan are the box running normally.

| Light | Pattern | Meaning |
|---|---|---|
| All three front | Blue-violet-cyan sweep, then cyan blink | Switched on, ready |
| All three front | One violet blink | NFC tap received |
| Middle | Three cyan blinks | Trip started |
| Middle | Short cyan blink every 15 min | Trip running normally |
| Middle | One blue blink | Trip stopped |
| Left | Two red blinks + three beeps | Temperature alarm |
| Left | One blue blink | Alarm acknowledged |
| Right | Three amber blinks | A problem with the box: probe, low battery, clock without time |

When someone lifts or moves the box, it shows its status lights briefly
(a few times an hour at most, to save the battery).

## 7. Trips

### Starting a trip

The acceptable temperature range (for example 2–8 °C) is set when each
trip starts, so the same box can carry different goods. When the trip has
started, the middle light blinks cyan three times and the screen shows
the trip icon.

A trip will not start if another trip is running, the battery is too low,
or the storage is full with nothing that may be deleted.

### During a trip

- Temperature is recorded every 5 minutes, with battery, position and how
  often the box was moved.
- If the box is switched off or reset on the way, the trip carries on when
  it comes back; a new trip is never started in its place.

### Alarms

| Alarm | Raised when | Cleared when |
|---|---|---|
| Temperature high / low | Out of range for longer than the set time (normally 5 min) | Back inside the range by 0.5 °C |
| Probe | No reading from the probe for over 1 minute | Readings return |
| Low battery | Battery below 15% | Battery back to 20% |

Acknowledging an alarm stops the sound and records that someone has seen
it; the alarm history is kept in full.

### Stopping a trip

The middle light blinks blue once, and the screen shows the trip's
summary for about an hour.

## 8. Connections

**NFC:** tap a phone on the NFC point. The front lights blink violet and
the box opens Bluetooth for the app for 60 seconds. The tap is also the
permission: no pairing, no code.

**Wi-Fi:** the box remembers up to 5 networks and joins the strongest. Staff
set them up from the app or the server, each with an automatic (DHCP) or a
fixed address. On battery it connects briefly every 5 minutes to send new
records; if it cannot, it waits longer between tries to save the battery.
Records waiting to be sent stay on the box.

**Time:** the box sets its clock from the internet or from satellites, and
the clock keeps running while the box is off.

## 9. Data on the box

- About 268 days of records at one every 5 minutes.
- Records leave the box only after the server confirms it has stored them.
- When storage is full, the oldest trips the server has confirmed go first.
- Nothing is lost when power is cut or the battery runs out.

## 10. Troubleshooting

| Problem | What to do |
|---|---|
| Screen shows BATTERY EMPTY / box will not switch on | Plug in USB-C; the box switches itself on |
| A trip will not start | Charge the battery; stop the previous trip first |
| Right light blinks amber | Check the probe is plugged in firmly; charge. If it persists, call staff |
| Screen shows a red `--` | The probe is unplugged or faulty: check it |
| Wi-Fi icon always crossed out | Out of range of the set networks; records are kept on the box |
| Side light blinks red | Unplug and plug in again. If it persists, call staff |

---

## Not yet included in this draft

- App steps: start/stop a trip, set the range, acknowledge alarms, view data (app not finished)
- Run time per charge and full-charge time (being measured)
- Specifications: size, weight, operating temperature, water resistance, approvals
- Pictures of the box and where the switch / port / NFC point are
- Four-colour screen (panel not in hand), shock alarm (threshold not set), door sensor (switched off)
- Reading data over USB drive / SD card, and firmware updates (deferred)
- Temperature accuracy (calibration pending)
