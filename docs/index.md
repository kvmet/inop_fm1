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
- The firmware measures each engine's slowest block at start-up. The number of voices depends on what is playing, up to 8.
- A track can limit how many voices it holds, such as 1 for a mono bass.

### Priority

Priority has 7 levels: XXLO, XLO, LOW, NORM, HIGH, XHI, XXHI. The sound sets a base value, and a lock can change it on one step.

When a trigger needs budget, it frees voices in this order until it fits:

1. Voices that are nearly silent.
2. Voices of lower priority, quietest first.
3. Voices of equal priority, oldest first.

A trigger never steals from a higher priority. If it still does not fit, it does not sound.

A stolen voice fades out over one audio block, and the new voice starts in the next block. A trigger that steals can start up to 2.9 ms late.

## LFOs

- Each LFO has a shape and a phase mode. The shapes are sine, triangle, saw, square and S&H.
- In retrigger mode each voice starts its own LFO. In free mode all voices share one clock, so they agree.
- Rate 0 is one cycle in 20 s, and the rate doubles every 12 values.
- LFO rate is not yet synced to BPM.

## Audio path

Each voice runs from its packet in this order: engine, drive, shaper, bit crush, rate reduce, filter, amp envelope, level, pan, sends. An effect at 0 costs no CPU. The filter type is off, low-pass, band-pass or high-pass; off costs no CPU.

- **Effects per trigger:** cheap distortion, such as a wave shaper, bit crush and sample rate reduction.
- **Shared effects, reached by sends:** delay, reverb and a higher quality distortion.
- **Master:** mix compression and mastering, then a limiter and soft clip that bound the output level.
- **Envelope follower sends:** a voice sends to a follower instead of an audio output. The follower is a modulation source for any voice.

Shared effects and the master have a fixed CPU cost, which is reserved before voices get budget.

## Modulation

- Each packet has free modulation slots. A slot holds a source, a destination and an amount.
- The sources are the mod envelope, LFO 1, LFO 2 and velocity. The envelope follower is added later.
- The destinations are pitch, cutoff, resonance, level, pan, drive, shaper, bit crush and rate reduce. An amount of 127 and a full source move a value by 127; pitch moves 16 semitones.
- A new sound starts with mod envelope to cutoff, LFO 1 to pitch, LFO 2 to cutoff and velocity to level, all at amount 0. Each slot can be changed.

## Packet

First version, about 56 values. KNOB1 to KNOB4 hold each page's main four values. SEL and ALGO hold extras when a page has them.

| Page | SEL | Knob 1 | Knob 2 | Knob 3 | Knob 4 | ALGO |
| --- | --- | --- | --- | --- | --- | --- |
| Home | pan | note | velocity | length | level | priority |
| Amp envelope | | attack | decay | sustain | release | |
| Mod envelope | | attack | decay | sustain | release | |
| Filter | | type | cutoff | resonance | key tracking | |
| Effects | delay send | shaper | bit crush | rate reduce | drive | reverb send |
| LFO 1, LFO 2 | | shape | phase mode | rate | start phase | |
| Mod slots | slot number | source | destination | amount | | |
| Engine 1 | engine | E1 | E2 | E3 | E4 | patch or model |
| Engine 2 | | E5 | E6 | E7 | | |

The engine sets what the 8 engine values mean. The mod slots page lists all 4 routes. The distortion and follower sends have no knob until they do something.

Delay and reverb settings are project values and are not in the packet. GLO shows them, the tempo, and how many voices of each engine and model fit the budget.

| Page | SEL | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- | --- |
| Global | BPM | delay time, 1 to 16 steps | delay feedback | reverb size | reverb damping |

The delay holds at most 0.74 s. A longer setting stops at that length. A change of time glides, so it bends pitch and does not click. The delay is mono, and the reverb is stereo.

## Sound pool

- All patterns in a project share one sound pool.
- A track's base is a reference to a pool sound.
- A step can have a sound lock, which replaces the base for that trigger. Ordinary locks apply on top.
- PRESETS selects the track's base sound, or the sound lock of a held step.
- A sound holds a default note, velocity and length. A step uses them until it sets its own, so a drum track can be tuned in one move.
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
- **Overdub:** press PLAY and REC together, in either order. The playing pattern takes what you do on the selected track, and it keeps playing. Tap PLAY to exit.
  - **Notes:** a key sounds at once and goes to the nearest step of the track. That step is turned on and takes the note and the held length. A step holds one note, so a second note on a step replaces the first.
  - **Black keys:** tracks cannot change during overdub, so the black keys are notes too. The last black key stays shift. To change tracks, exit and enter again.
  - **Knobs:** the knob squares show the values of the track's last trigger, so locks show as they play. A turned knob starts from the value shown and writes its value to each step the playhead reaches. It stops when the knob has been still for one step or 150 ms, whichever is longer. Hold REC to keep writing the knobs turned so far, then release REC to stop.
  - **Locks:** a lock changes only its step's trigger, so a step that is off takes no lock, and a sweep does not change a note already sounding. `LOCKS FULL` shows when the track has no lock memory left.
  - **Other controls:** the sound, global and track values change as usual and are not recorded. SEQ does nothing. There is no undo yet.
- **Copy a sound:** hold REC and turn PRESETS to a slot. The screen shows the slot and how many tracks use that slot. Release REC, then tap REC to confirm. Any other press or turn cancels and does nothing else. After a copy, the track uses the new slot.

## Keyboard

- **Compose (default):** tap a white key to toggle its step.
- **Play:** tap PLAY. White keys play notes, and nothing is recorded. Tap PLAY again to return to compose. PLAY acts on release, so that PLAY and REC together can start overdub.
- **Black keys:** OP1 to OP6 select a track in every state but overdub. Hold one for 300 ms to show the track page while it is held: knob 1 sets the track length, and knob 2 sets the base sound.
- **OCT:** in compose, OCT moves the steps that the white keys reach. In play, OCT moves the keys by an octave, up to 3 each way. The bottom line of the screen says which.
- **Shift:** hold the last black key to move numeric values by 10 per click. Selectors still move by 1. Shift is off in pattern select, where that key is a pattern.
- **Patterns:** tap SEQ for pattern select, and tap SEQ again to leave. Each of the 27 keys is one pattern, with no banks. Hold REC there to copy a pattern: press the source, then each destination.
- **Held steps:** hold a white key for 300 ms or more to view its step's values. Releasing changes nothing. Several steps can be held. A knob move changes every held step by the same amount and turns those steps on. The screen shows the values of the step held longest. The note knob sets pitch.
- **Step LEDs:** steps 1-4 and 9-12 show brighter than 5-8 and 13-16, to match the screen. An empty step is about 2/30 of full or off, and a step that is on is full or half. A step with a dot on the screen changes level briefly about once a second.

## Screen

- **Layout:** 1 px borders mark each area. Top to bottom: title bar, knob squares and free space, sequence, bottom row.
- **Title bar:** mode, page, track, track sound, pattern, counter and BPM. `P3>1` means pattern 3 is shown and pattern 1 plays. The counter `48.15` means the 48th play and step 15 of the playing pattern. One play ends when the pattern's longest track completes. The play number wraps from 64 to 1 and starts at 1 when a committed pattern starts.
- **Knob squares:** row 1 is SEL and knobs 1 to 4, as on the panel, with each name over its value. Row 2 is ALGO, under SEL.
- **Free space:** to the right of ALGO. The page sets what it shows. A sound copy prompt replaces it while it is active.
- **Bottom row:** the OCT hint on the left. On the right, CPU, active voices, and L, D and S: late audio buffers, dropped triggers and stolen voices. These three show 2 digits and wrap, so a change shows that it is happening. A held step's values replace them while it is held.
- **Sequence:** below the knobs, one row of steps per track, for the pattern being composed or previewed. All tracks use one step width, so shorter tracks end early. A playhead shows only when the shown pattern is playing. Each row has a square for each step and a lane under the squares for the playhead. A step with locks, or with a changed note, velocity or length, has a dot in its square. The selected track's number is inverted. A bar under its steps marks the ones that the white keys reach.
- **Page buttons:** the first press shows the group's last page. Each next press moves through the group.
- **Knobs:** with no step held, they edit the track's sound. With a step held, they edit that step.
- **Counts:** D counts triggers that never sounded. S counts sounding voices that a new trigger cut short.

| Page | Free space |
| --- | --- |
| Home | Peak meters: master left and right, delay send, reverb send. A full bar is the limiter's ceiling, and 1/8 of a bar is 6 dB. |
| Amp envelope, Mod envelope | The envelope's shape, with a fixed width for the sustain. The curve is the envelope code's own output. One pixel is 0.73 ms until the curve fills the width, and then the curve scales to fit. |
| LFO 1, LFO 2 | Two cycles of the waveform at its start phase. |
| Mod slots | All 4 routes. |
| Engine 1, Engine 2 | FM: the patch's algorithm and name, with carriers solid and modulators outlined. Physical modeling: the model. Perc: each partial as a bar, at its frequency and starting level. |
| Global | Voices of each engine and model that fit the budget. |

| Button | Pages |
| --- | --- |
| HOME | Home |
| SEL | Engine 1, Engine 2 |
| ARP | Filter |
| ENV | Amp envelope, Mod envelope |
| FX | Effects |
| LFO | LFO 1, LFO 2, Mod slots |
| GLO | Global |

## Engines

- The first pass has an FM engine, a physical modeling engine and the Perc drum engine. More engines are added later.
- The physical modeling models are modal, string, membrane and sympathetic strings.
- The sound's amp envelope ends a physical modeling voice. Keep sustain high to hear the model's own decay.
- The engine choice is in the packet, so a trigger can choose its engine.
- There are no samples and no sample slots.

### Perc

Six sine partials, each with its own decay, and noise are summed. The sum then passes through the onset, the fold and a gain that restores the decay that folding removes. A Perc voice ignores note length and ends when its partials end. The voice chain applies after it as usual.

| Knob | Page | Value |
| --- | --- | --- |
| ALGO | Engine 1 | Mode. SKIN: the partials are independent. METAL: each partial modulates the next. |
| 1 | Engine 1 | Decay: the time to -60 dB |
| 2 | Engine 1 | Spread: partial spacing. 0 is unison, 64 is the harmonic series, and other values are inharmonic. |
| 3 | Engine 1 | Harm: low values make upper partials quiet and short, and high values make them loud and ringing. |
| 4 | Engine 1 | Fold |
| 1 | Engine 2 | Bend: how far the pitch starts above the note, up to 47 semitones |
| 2 | Engine 2 | Noise: level, up to half of the sum |
| 3 | Engine 2 | Attack: low values add a click, and high values make a longer onset, up to 46 ms |

## Roadmap

- Sound names, entered on the keyboard: 26 letters and an underscore on the 27 keys.
- Clear an entire track.
- Randomize an entire track: its triggers, or one chosen parameter.
- Track mutes.
- Clear and randomize during overdub, including a way to unset locks and free their memory.
- Chord modes.
- Quantization: pitch to a scale, and recorded notes to steps.
- Filter envelope, possibly as more assignable envelopes in the mod slots.
- Arpeggiator.
- More engines. Take ideas from Digitone, Noise Engineering BIA, MicroFreak, Acid Rain Ripsaw and VHIKK X.
- To consider for engines: a comb filter type (Digitone II), a metallic delay and reverb (VHIKK X), wave morph and buzz pulses at high fold (BIA), and speech, harmonic and bass engines (MicroFreak).
- After the engines, a page review: the page plan, easier mod slot editing, better use of the buttons, and what the screen shows in each use, with smaller meters.
- Master compression and mastering.

## Open

- Steps per track and locks per track. The first estimate is 8 KB a pattern: 64 steps and about 270 locks per track.
- Whether a step holds several notes, for recording and chords. Decide this before saving to flash.

- How many envelope followers there are.
- How much lock storage a track gets. A track with every step locked must fit.
