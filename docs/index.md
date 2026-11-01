# INOP FM-1 model

## Tracks

- A track is one row of steps with one base sound.
- All tracks are the same. There are no track types.
- There are 6 tracks, selected by the black keys labeled OP1 to OP6. The count can change.
- Variety on a track comes from parameter locks.

## Triggers

- A step stores sparse locks. A trigger resolves the base sound and those locks into one complete packet.
- A voice renders only from its packet, so overlapping triggers can be different sounds.

## Voices

- Voices are one pool shared by all tracks.
- There is no voice assignment page. A voice holds its engine only while its trigger lasts.
- The firmware measures each engine's slowest block at start-up. The number of voices depends on what is playing, up to 16.
- A track can limit how many voices it holds, such as 1 for a mono bass.

### Note length

A step or sound sets how many steps its note lasts, up to 64. One more turn of the knob is `HOLD`: the note sounds until the track's next note. The same note again keeps sounding, so a pattern can loop under it. A different note, a changed value or a new lock starts a new note. A held note also ends when its track has no steps on. Its values come from the trigger, so a knob change takes effect at the next trigger.

### Priority

Priority has 7 levels: XXLO, XLO, LOW, NORM, HIGH, XHI, XXHI. The sound sets a base value, and a lock can change it on one step.

When a trigger needs budget, it frees voices in this order until it fits:

1. Voices of equal or lower priority that are nearly silent after their attack.
2. Voices of lower priority, quietest first.
3. Voices of equal priority, oldest first.

A trigger never steals from a higher priority, even from a voice that is nearly silent or has not sounded yet. If it still does not fit, it does not sound, and the `D` counter rises.

A stolen voice fades out over one audio block, and the new voice starts in the next block. A trigger that steals can start up to 2.9 ms late.

## LFOs

- Each LFO has a shape and a phase mode. The shapes are sine, triangle, saw, square and S&H.
- In retrigger mode each voice starts its own LFO. In free mode all voices share one clock, so they agree. In sync mode all voices read the sequencer's place, so they agree and stay on the grid.
- S&H agrees across voices in free and sync modes: a voice that starts late has the value the others hold.
- Rate 0 is one cycle in 20 s, and the rate doubles every 12 values.
- In sync mode, rate picks a cycle length: 1/32, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2T, 1/2, then 1, 2, 4, 8 or 16 bars of 16 steps. It follows the tempo. Stop rewinds it, and a pattern change does not.

## Audio path

Each voice runs from its packet in this order: engine, drive, shaper, bit crush, rate reduce, filter, amp envelope, level, pan, sends. An effect at 0 costs no CPU. The filter type is off, low-pass, band-pass or high-pass; off costs no CPU.

- **Effects per trigger:** cheap distortion, such as a wave shaper, bit crush and sample rate reduction.
- **Shared effects, reached by sends:** delay, reverb and a higher quality distortion.
- **Master:** in order: tilt, compressor, width, drive, limiter, soft clip, volume. See Master below.
- **Envelope follower send:** a voice sends to the follower, and the follower is a modulation source for every voice. See Follower below.

Shared effects and the master have a fixed CPU cost, which is reserved before voices get budget. A SWARM voice costs more at higher notes and with Sync, since its work follows its waveform edges. It is charged for its note. The System page shows what each engine, effect and shared stage costs.

## Modulation

- Each packet has free modulation slots. A slot holds a source, a destination and an amount.
- A packet has 8 slots. Routes to one destination add up.
- The sources are mod envelope 1, mod envelope 2, LFO 1, LFO 2, velocity, random and the follower. Random draws one value per trigger and holds it for the note, from -1 to 1 of full scale.
- The destinations are pitch, cutoff, resonance, level, pan, drive, shaper, bit crush, rate reduce, timbre, FM index and the rate of each LFO. A rate route works in retrigger mode only: a free LFO keeps its rate. An amount of 127 and a full source move a value by 127; pitch moves 16 semitones.
- A new sound starts with mod envelope 1 to cutoff, LFO 1 to pitch, LFO 2 to cutoff and velocity to cutoff, all at amount 0, and mod envelope 1 to FM index at 127. Each slot can be changed.
- **FM index:** routes to it scale the index, so the index knobs set the most it can reach, and an envelope at 127 reaches that at its peak. With no route, the index stays at its knobs. A route with amount 0 closes it. To have no route, set its source to off.

### Hold to route

Hold LFO or ENV and turn a knob on any page. That knob's value does not change. The turn sets the amount of a route from that source to that value, and the bottom row shows it, such as `LF1>CUT +12`. If the route exists, the turn moves its amount. If not, it takes the first slot with no source, no destination or amount 0, starting at amount 0.

- **Source:** the page last shown under the button. LFO gives LFO 2 if that page was last, else LFO 1. ENV gives mod envelope 2 if that page was last, else mod envelope 1.
- **Knobs with a route:** note, cutoff, resonance, level, pan, drive, shaper, bit crush, rate reduce, each LFO's rate, FM's index knobs, and the engine knob that timbre moves: Perc FM, SWARM SHAPE and WAVE WAVE. Others show `NO ROUTE`.
- **Held steps and overdub:** with a step held, the route is a lock on that step. In overdub, the turn records the route as locks on the steps it passes.
- **No free slot:** the bottom row shows `SLOTS FULL`.
- **Page change:** LFO and ENV change the page on release, and not after a route.

## Follower

The follower turns audio into a modulation source, such as a kick that ducks the pads.

- **Send:** FOLLOW on the Sends page sets how much of a voice goes to the follower. The send is taken after the amp envelope and before LEVEL, so a track at level 0 can still drive it. The loudest send of a block wins.
- **Source:** `FOL` in a mod slot goes from 0 to full. Route it to `LVL` with a negative amount to duck.
- **Timing:** ATK and REL on the Follower page set its attack, 0.7 to 186 ms, and its release, 29 ms to 1.8 s. They are project values. Every voice reads the value of the block before, so it reads 0.7 ms late, whatever the voice's place among the voices.
- **Follower page:** a bar shows the follower's level.

There is one follower.

## Packet

First version, about 56 values. KNOB1 to KNOB4 hold each page's main four values. SEL and ALGO hold extras when a page has them.

| Page | SEL | Knob 1 | Knob 2 | Knob 3 | Knob 4 | ALGO |
| --- | --- | --- | --- | --- | --- | --- |
| Home | pan | note | velocity | length | level | priority |
| Amp envelope | | attack | decay | sustain | release | |
| Mod envelope 1, Mod envelope 2 | | attack | decay | sustain | release | |
| Filter | | type | cutoff | resonance | key tracking | |
| Effects | delay send | shaper | bit crush | rate reduce | drive | reverb send |
| Sends | | delay send | reverb send | follower send | | |
| LFO 1, LFO 2 | | shape | phase mode | rate | start phase | |
| Mod slots | slot number | source | destination | amount | | |
| Engine 1 | engine | E1 | E2 | E3 | E4 | patch or model |
| Engine 2 | | E5 | E6 | E7 | | |

The engine sets what the 8 engine values mean. The mod slots page lists all 8 routes. The distortion send has no knob until it does something.

Delay and reverb settings are project values and are not in the packet. The Global page shows them and the tempo.

| Page | SEL | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- | --- |
| Global | BPM | delay time, 1 to 16 steps | delay feedback | reverb size | reverb damping |

The delay holds at most 0.74 s. A longer setting stops at that length. A change of time glides, so it bends pitch and does not click. The delay is mono, and the reverb is stereo.

## Master

The Master page is on GLO. Its values are project values: steps do not lock them, and they do not change with the pattern. A stage at its neutral value costs no CPU, and the budget reserves what all stages cost at full settings. The System page shows that as `MST`.

| Knob | Name | Range | Neutral | What it does |
| --- | --- | --- | --- | --- |
| SEL | WIDTH | 0 to 127 | 64 | Stereo width. 0 is mono, and 127 about doubles the side signal. |
| Knob 1 | COMP | 0 to 127 | 0 | How hard the compressor works. It lowers the threshold from the limit to 30 dB under it, raises the ratio from 1.5:1 to 4:1, and adds back half of what a signal at the limit loses. The knee is 6 dB wide. |
| Knob 2 | ATK | 0 to 127 | | Attack, 0.7 to 186 ms. |
| Knob 3 | REL | 0 to 127 | | Release, 29 ms to 1.8 s. |
| Knob 4 | TILT | -63 to 63 | 0 | Above 0, the lows rise and the highs fall, up to 6 dB each, around 700 Hz. Below 0, the reverse. |
| ALGO | DRIVE | -48 to 48 | 0 | Gain into the limiter, 6 dB per 12 units, so up to 24 dB each way. Use below 0 to keep a hot mix out of the limiter. |

The compressor links left and right and moves its gain once per block, on the peak of the block that the gain applies to. It sees the signal after TILT.

The free space shows the compressor's curve for a steady level, with the diagonal as no change, and bars for the gain reduction of the compressor and the limiter. A full bar is 12 dB.

The MASTER knob is the last stage, so it sets the volume and does not change how hard the other stages work. A level above the limit goes to the limiter at any knob position.

## Sound pool

- All patterns in a project share one sound pool.
- A track's base is a reference to a pool sound.
- A step can have a sound lock, which replaces the base for that trigger. Ordinary locks apply on top.
- PRESETS selects the track's base sound, or the sound lock of a held step.
- A sound holds a default note, velocity and length. A step uses them until it sets its own, so a drum track can be tuned in one move.
- Velocity sets the peak of the amp envelope: 127 is full and 64 is -12 dB. Level then sets the sound's overall level.
- Editing a sound changes it everywhere it is used.

## Patterns

- Selecting a pattern shows its steps for editing. It plays nothing. The playing pattern keeps playing until you commit the new one.
- Tracks run free. Each track wraps on its own length, and tracks of different lengths drift against each other.
- A committed pattern starts when the longest track of the playing pattern completes. All tracks then start from step 1. This is the only time tracks restart.
- A switch does not interrupt sound. Voices that are ringing finish from their packets.
- The first press on a pattern's key selects it. A second press commits it.
- An edit to a pool sound can be heard in the playing pattern, on that sound's next trigger. The screen marks a sound that the playing pattern uses. To keep an edit apart, duplicate the sound first.

## REC

- **Hold:** copy. Press the source, then press each destination.
- **Clear locks:** hold a step and tap REC. The step keeps its trigger and loses its locks, its note, velocity and length, its sound and its chance. Every held step clears. There is no undo yet.
- **Overdub:** tap REC, under 300 ms with nothing else pressed. The playing pattern takes what you do on the selected track, and it keeps playing. Tap REC or PLAY to exit.
  - **Notes:** a key sounds at once and goes to the nearest step of the track. That step is turned on and takes the note and the held length. A step holds one note, so a second note on a step replaces the first.
  - **Black keys:** the black keys are notes too, and the last one stays shift. Hold REC and press OP1 to OP6 to change tracks. The key LEDs show the keyboard as in play, not the steps, and the selected track's key flashes. `DUB` in the title bar inverts in time with it.
  - **Knobs:** the knob squares show the values of the track's last trigger, so locks show as they play. A turned knob starts from the value shown and writes its value to each step the playhead reaches. It stops when the knob has been still for one step or 150 ms, whichever is longer. Hold REC to keep writing the knobs turned so far, then release REC to stop.
  - **Locks:** a lock changes only its step's trigger, so a step that is off takes no lock, and a sweep does not change a note already sounding. `LOCKS FULL` shows when the track has no lock memory left.
  - **Other controls:** the sound, global and track values change as usual and are not recorded. SEQ does nothing. There is no undo yet.
- **Copy a sound:** hold REC and turn PRESETS to a slot. The screen shows the slot and how many tracks use that slot. Release REC, then tap REC to confirm. Any other press or turn cancels and does nothing else. After a copy, the track uses the new slot.

## Keyboard

- **Steps (default):** tap a white key to toggle its step. The title bar says `STEPS`.
- **Notes:** tap PLAY. White keys play notes, and nothing is recorded. The title bar says `NOTES`. Tap PLAY again to return to steps.
- **Stop:** hold shift and press PLAY. The sequencer stops at step 1, the voices fade at once, and the counter says `STOP`. Press again to start from step 1. A pattern chosen while stopped starts at once. The keys still play notes while stopped.
- **Black keys:** OP1 to OP6 select a track, except in overdub, where they need REC held. Hold one for 300 ms to show the track page while it is held: knob 1 sets the track length, and knob 2 sets the base sound.
- **OCT:** in steps mode, OCT moves the steps that the white keys reach. In notes mode, OCT moves the keys by an octave, up to 3 each way. The bottom line of the screen says which.
- **Shift:** hold the last black key to move numeric values by 10 per click. Selectors still move by 1. Shift is off in pattern select, where that key is a pattern.
- **Patterns:** tap SEQ for pattern select, and tap SEQ again to leave. Each of the 27 keys is one pattern, with no banks. Hold REC there to copy a pattern: press the source, then each destination.
- **Held steps:** hold a white key for 300 ms or more to view its step's values. Releasing changes nothing. Several steps can be held. A knob move changes every held step by the same amount and turns those steps on. The screen shows the values of the step held longest, and the values that step sets for itself have their own color. The note knob sets pitch. A copy or a knob move that runs out of lock memory shows `LOCKS FULL`.
- **Step LEDs:** steps 1-4 and 9-12 show brighter than 5-8 and 13-16, to match the screen. An empty step is about 2/30 of full or off, and a step that is on is full or half. A step with a dot on the screen changes level briefly about once a second.

## Screen

- **Layout:** 1 px borders mark each area. Top to bottom: title bar 20 px, knob squares 47 px, free space 72 px, sequence 79 px, bottom row 16 px.
- **Title bar:** mode, page, track, pattern, counter and BPM. `P3>1` means pattern 3 is shown and pattern 1 plays. The counter `48.05` means the 48th play and step 5 of the playing pattern, always with 2 digits each. One play ends when the pattern's longest track completes. The play number wraps from 64 to 1 and starts at 1 when a committed pattern starts.
- **Knob squares:** row 1 is SEL and knobs 1 to 4, as on the panel, with each name over its value. Row 2 is ALGO, under SEL.
- **Sound square:** under ALGO. `S12 x3` means the knobs edit sound 12, and 3 tracks across all patterns use it, so all 3 hear a change.
- **Free space:** to the right of ALGO and the sound square. The page sets what it shows. A sound copy prompt replaces it while it is active.
- **Unused knobs:** blank, and turning them does nothing.
- **Page tabs:** when a page button has more than one page, its lower edge shows one segment per page. The shown page's segment is solid. Press the page button again for the next page.
- **Bottom row:** the OCT hint on the left. On the right, in fixed places, `CPU 42 V 08 D00`: load, active voices and dropped triggers. D wraps at 100 and flashes for 1 s on a new drop. A held step's values replace them while it is held. The System page shows late audio buffers and stolen voices.
- **Sequence:** below the free space, one row of steps per track, for the pattern being edited or previewed. All tracks use one step width, so shorter tracks end early. A playhead shows only when the shown pattern is playing. Each row has a square for each step and a lane under the squares for the playhead. A step with locks, or with a changed note, velocity, length or chance, has a dot in its square. The selected track's number is inverted. A line in its lane marks the steps that the white keys reach.
- **Page buttons:** the first press shows the group's last page. Each next press moves through the group. HOME has one page, so it always lands there.
- **Knobs:** with no step held, they edit the track's sound. With a step held, they edit that step.
- **Counts:** D counts triggers that never sounded. STOLEN on the System page counts sounding voices that a new trigger cut short.

| Page | Free space |
| --- | --- |
| Home | Peak meters: master left and right, delay send, reverb send. A full bar is the limiter's ceiling, and 1/8 of a bar is 6 dB. |
| Amp envelope, Mod envelopes | The envelope's shape, with a fixed width for the sustain. The curve is the envelope code's own output. One pixel is 0.73 ms until the curve fills the width, and then the curve scales to fit. |
| Filter | The response on the CUT knob's scale, with a peak that follows RES. A sketch at 12 dB an octave, not a measurement. Key tracking does not show. |
| LFO 1, LFO 2 | One second of the waveform at its start phase, so a faster rate shows more cycles, up to one cycle per 7 px. |
| Mod slots | All 8 routes in 2 columns. The selected slot is brighter. |
| Engine 1, Engine 2 | FM: the algorithm, with carriers solid and modulators outlined. Each box names the ratio knob that sets it: C, A or B. Feedback is marked on B2. To its right, one cycle from the engine at full index. Perc: each partial as a bar, at its frequency and starting level. SWARM and WAVE: the wave. |
| Step | `HOLD A STEP` until a step is held. |
| System | Cost in us of one block: each engine alone and shared, each effect, delay, reverb and master, and the budget. Then late buffers, stolen voices and drops. |
| Master | See Master. |
| Arp, Save | `NOT BUILT`. Save warns that power off loses everything. |

| Button | Pages |
| --- | --- |
| HOME | Home |
| SEL | Engine 1, Engine 2 |
| EDIT | Step |
| ENV | Amp envelope, Mod envelope 1, Mod envelope 2 |
| FX | Filter, Effects, Sends |
| LFO | LFO 1, LFO 2, Mod slots, Follower |
| GLO | Global, Master, System |
| ARP | Arp (reserved for a Perform and Arp group) |
| SAVE | Save |

The Step page's knob 1 sets the chance that a held step fires, 0 to 100 %. Knobs do nothing there with no step held.

A sound keeps its engine values for each engine it has used. A new engine starts from its defaults, and sweeping back restores the old values. A lock on the engine changes only that step and does not use this memory.

## Engines

- The engines are FM, Perc, SWARM and WAVE.
- The sound's amp envelope ends a voice on every engine.
- The engine choice is in the packet, so a trigger can choose its engine.
- **TIMBRE:** a mod slot destination that moves each engine's main value: Index A and B on FM, FM on Perc, Shape on SWARM and Wave on WAVE.
- **SHAPE (FX page):** a wavefolder on every engine, up to 16 folds of drive. It starts folding at low values, and the output follows the input's level, so a sound keeps its loudness and its decay. It runs before DRIVE.
- There are no samples and no sample slots.

### FM

Four sine operators, C, A, B1 and B2, in one of 8 algorithms. `>` means modulates. C always sounds.

| Algorithm | Routes | Sounding operators |
| --- | --- | --- |
| 1 | B2 > B1 > A > C | C |
| 2 | B2 > B1 > C, A > C | C |
| 3 | A > C, B1 > C, B2 > C | C |
| 4 | B1 > A > C, B2 > A | C |
| 5 | A > C, B2 > B1 | C, B1 |
| 6 | B2 > C, B2 > A, B2 > B1 | C, A, B1 |
| 7 | A > C | C, B1, B2 |
| 8 | none | all |

The MOD envelope scales every operator but C, so the MOD page sets how the timbre moves over a note. Index A sets A, and Index B sets B1 and B2. Each index is a modulation depth for a modulator and a level for a sounding operator, so a sounding A, B1 or B2 follows the MOD envelope too. Use MOD SUS to keep it sounding. The amp envelope shapes the whole voice.

| Knob | Page | Value |
| --- | --- | --- |
| ALGO | Engine 1 | Algorithm |
| 1 | Engine 1 | Ratio C: 1/4, 1/2, or a whole number up to 16 |
| 2 | Engine 1 | Ratio A |
| 3 | Engine 1 | Ratio B: B1 and B2 |
| 4 | Engine 1 | Index A |
| 1 | Engine 2 | Index B |
| 2 | Engine 2 | Feedback on B2 |
| 3 | Engine 2 | Detune: A and B2 rise and B1 falls, by up to 1 semitone, so ratios stop being whole numbers |

### SWARM

Seven oscillators around the note, each with its own start phase, and a sine sub. The oscillators beat against each other, which suits a drone.

| Knob | Page | Value |
| --- | --- | --- |
| ALGO | Engine 1 | Stack. `SAME`: all at the note. `OCTS`: two oscillators an octave down, three at the note, and two an octave up. |
| 1 | Engine 1 | Detune: up to 11 % each way, with fine control at the low end |
| 2 | Engine 1 | Shape: 0 is a saw, 64 is a square, and 127 is a narrow pulse. The loudness stays about the same. |
| 3 | Engine 1 | Sync: 0 is off. Above that, each oscillator hard-syncs a second one, up to nearly 4 times its pitch. |
| 4 | Engine 1 | Sides: the level of the six outer oscillators against the center one. 0 is the center alone. |
| 1 | Engine 2 | Sub: the level of a sine an octave below the note. 0 is off. It does not depend on Sides or Shape, and it does not change them. |
| 2 | Engine 2 | Drift: slow random movement of each oscillator's pitch, up to 1.5 %, and of its pulse width |

The screen shows one cycle of the wave with its sync restarts. Under it is one tick per oscillator: across for detune, a row per octave for stack, and height for level. The bar below them widens with Sub.

### WAVE

Two oscillators read the same wavetable, one cycle at a time, and noise is added. Each table has 16 frames, and notes read a copy with only the harmonics that fit under Nyquist. The tables are `HARM`, `PWM`, `SYNC`, `VOX`, `FOLD`, `DIGI`, `METAL` and `GRIT`.

| Knob | Page | Value |
| --- | --- | --- |
| ALGO | Engine 1 | Table |
| 1 | Engine 1 | Wave: position in the table, moving smoothly between frames |
| 2 | Engine 1 | Warp: bends the phase of both oscillators as they read, which adds harmonics |
| 3 | Engine 1 | Pitch2: oscillator 2's pitch, -24 to +24 semitones |
| 4 | Engine 1 | Cross: 0 is off. Below 0, oscillator 2 ring-modulates oscillator 1. Above 0, it frequency-modulates it. |
| 1 | Engine 2 | Osc2: oscillator 2's own level. 0 is silent, and Cross still works. |
| 2 | Engine 2 | Detune: oscillator 2's fine offset, up to 1 semitone |
| 3 | Engine 2 | Noise: level |

The screen shows one cycle of oscillator 1 as Wave and Warp set it, over a tick for each frame. Warp and Cross add harmonics that the tables do not limit, so extreme settings alias on high notes.

### Perc

Six sine partials and noise are summed. The voice chain applies after that as usual, and its SHAPE folds the sum.

The amp envelope means the same as on every other engine:

- **ATK:** the rise of the voice.
- **DEC:** the partials' time to -60 dB. Harm shortens the upper partials. FM depth falls in half of this time. Noise falls with the bend, in 170 ms plus a quarter of this time, and never takes longer than this time.
- **SUS:** the level that the partials and FM depth fall to. Noise always falls to nothing. At 0, the voice ends when the partials end.
- **REL:** the fade after the note ends. For a one-shot drum, set SUS to 0 and REL high: the voice rings for DEC whatever the note length. A short REL makes a gated sound that follows note length.

| Knob | Page | Value |
| --- | --- | --- |
| ALGO | Engine 1 | Mode: the partial frequencies. TONE: harmonics. SKIN: membrane modes. BAR: free bar modes. METAL: TR-808 cymbal oscillators. |
| 1 | Engine 1 | FM: how far each partial modulates the next. It does not depend on the partials' levels, so it works at Harm 0. |
| 2 | Engine 1 | Spread: partial spacing. 0 is unison, 64 is the mode's frequencies, and 127 is about twice as wide. |
| 3 | Engine 1 | Harm: low values make upper partials quiet and short, and high values make them loud and ringing. |
| 4 | Engine 1 | Bend: how far the pitch starts above the note, up to 47 semitones |
| 1 | Engine 2 | Noise: level, up to half of the sum |
| 2 | Engine 2 | Click: a noise burst at the start |

## Test probe

A build with `INOP_PROBE=1` adds a USB serial port and a USB audio input, so a computer can run the device without hands. `tools/inop_probe.py` presses buttons and keys, turns knobs, loads patterns, reads CPU and voice counters, saves screenshots and records the master.

```bash
python3 tools/inop_probe.py 'tap B_LFO' 'knob 1 5' 'shot /tmp/page.png' stat
```

- Commands enter below the key debounce, so they run the same UI code as the panel.
- A recording takes the full level. The MASTER knob sets only the speaker and headphone level.
- A screenshot redraws the whole screen, so the screen flashes and the UI waits for about a second. The sound continues.
- macOS 13 to 15 lose the audio input when the serial port is present. macOS 26 keeps both.
- A release build has none of this.

## Roadmap

- Sound names, entered on the keyboard: 26 letters and an underscore on the 27 keys.
- Clear an entire track.
- Randomize an entire track: its triggers, or one chosen parameter.
- Track mutes: POLY latches a mute mode where the OP keys toggle tracks, with dimmed rows and OP LEDs.
- One level of undo, with clear and randomize.
- Perform layer on ARP: density, chance offset, mutate and fill as offsets that do not change the pattern. PIT fill, GLO reset, MONO mutate.
- MIDI clock.
- Per-step parameter lane and range select.
- Clear and randomize during overdub, including a way to unset locks and free their memory.
- Chord modes.
- Quantization: pitch to a scale, and recorded notes to steps.
- Arpeggiator.
- Microtiming: a step fires ahead of or behind the grid.
- Swing.
- Ratchets: a step fires several times within its length.
- More engines. Take ideas from Digitone, Noise Engineering BIA, MicroFreak, Acid Rain Ripsaw and VHIKK X.
- To consider for engines: a comb filter type (Digitone II), a metallic delay and reverb (VHIKK X), wave morph and buzz pulses at high fold (BIA), and speech, harmonic and bass engines (MicroFreak).
- Multiband compression on the master.
- A second envelope follower, for separate sidechains.

## Open

- Steps per track and locks per track. The first estimate is 8 KB a pattern: 64 steps and about 270 locks per track.
- Whether a step holds several notes, for recording and chords. Decide this before saving to flash.

- Bug to find: with a NORM snare firing on every step, a kick at HIGH did not fire every time. A trigger of higher priority should take a lower one on any track, and a test of that rule on the voice pool passes.
- How much lock storage a track gets. A track with every step locked must fit.
