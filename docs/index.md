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
- Each engine has a measured worst-case CPU cost. The number of voices depends on what is playing.
- A track can limit how many voices it holds, such as 1 for a mono bass.

### Priority

Priority is low, normal or high. The track sets a base value, and a lock can change it on one step.

When a trigger needs budget, it frees voices in this order until it fits:

1. Voices that are nearly silent.
2. Voices of lower priority, quietest first.
3. Voices of equal priority, oldest first.

A trigger never steals from a higher priority. If it still does not fit, it does not sound.

A stolen voice fades out over one audio block, and the new voice starts in the next block. A trigger that steals can start up to 2.9 ms late.

## LFOs

- Each LFO has a selectable mode, such as S&H, retrigger or free-running.

## Audio path

Each voice runs its engine, filter, envelopes and cheap effects from its packet. Then it goes to level, pan and sends.

- **Effects per trigger:** cheap distortion, such as a wave shaper, bit crush and sample rate reduction.
- **Shared effects, reached by sends:** delay, reverb and a higher quality distortion.
- **Master:** mix compression and mastering.
- **Envelope follower sends:** a voice sends to a follower instead of an audio output. The follower is a modulation source for any voice.

Shared effects and the master have a fixed CPU cost, which is reserved before voices get budget.

## Modulation

- Each packet has free modulation slots. A slot holds a source, a destination and an amount.
- Sources include LFOs, the mod envelope, velocity and the envelope follower.
- Any continuous packet value can be a destination.
- A new sound starts with useful routes in its slots, and each slot can be changed.

## Packet

First version, about 56 values. Each group of 4 fits the 4 knobs.

| Page | Knob 1 | Knob 2 | Knob 3 | Knob 4 |
| --- | --- | --- | --- | --- |
| Trigger | note | velocity | length | priority |
| Amp envelope | attack | decay | sustain | release |
| Filter | type | cutoff | resonance | key tracking |
| Mod envelope | attack | decay | sustain | release |
| Voice effects | shaper | bit crush | rate reduce | drive |
| Mix | level | pan | | |
| Sends | delay | reverb | distortion | follower |
| LFO 1 | shape | phase mode | rate | start phase |
| LFO 2 | shape | phase mode | rate | start phase |
| Slot source | slot 1 | slot 2 | slot 3 | slot 4 |
| Slot destination | slot 1 | slot 2 | slot 3 | slot 4 |
| Slot amount | slot 1 | slot 2 | slot 3 | slot 4 |

The packet also holds the engine choice and 8 engine values on 2 pages. The engine sets what those values mean.

## Sound pool

- All patterns in a project share one sound pool.
- A track's base is a reference to a pool sound.
- A step can have a sound lock, which replaces the base for that trigger. Ordinary locks apply on top.
- Editing a sound changes it everywhere it is used.

## Patterns

- Selecting a pattern shows its steps for editing. It plays nothing. The playing pattern keeps playing until you commit the new one.
- A committed pattern starts when the longest track of the playing pattern completes. All tracks then start from step 1.
- A switch does not interrupt sound. Voices that are ringing finish from their packets.
- The first press on a pattern's key selects it. A second press commits it.
- An edit to a pool sound can be heard in the playing pattern, on that sound's next trigger. The screen marks a sound that the playing pattern uses. To keep an edit apart, duplicate the sound first.

## REC

- **Hold:** copy. Press the source, then press each destination.

## Keyboard

- **Compose (default):** white keys toggle steps.
- **Play:** tap PLAY. White keys play notes, and nothing is recorded. Tap PLAY again to return to compose.
- **Black keys:** OP1 to OP6 select a track in every state.
- **Patterns:** tap SEQ for pattern select, and tap SEQ again to leave. Each of the 27 keys is one pattern, with no banks. Hold REC there to copy a pattern: press the source, then each destination.
- **Held step:** knobs write locks, and the note knob sets the step's pitch.

The first pass has no overdub state. Recording notes and knob moves live is added after the rest has been tested.

## Engines

- The first pass has an FM engine and a drum engine. More engines are added later.
- The engine choice is in the packet, so a trigger can choose its engine.
- There are no samples and no sample slots.

## Open

- How to select and assign pool sounds, and how to copy one.
- Steps per track and locks per track. The first estimate is 8 KB a pattern: 64 steps and about 270 locks per track.
- How to record notes and knob moves live.

- How many envelope followers there are.
- How much lock storage a track gets. A track with every step locked must fit.
