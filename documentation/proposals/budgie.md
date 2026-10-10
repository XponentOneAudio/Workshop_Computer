# Proposal: Budgie — a card that speaks budgerigar

*Status: proposal / research notes. No card number claimed yet.*

The aim is a Workshop Computer card that **explores the structure of budgerigar
(*Melopsittacus undulatus*) vocal communication**, and builds on that to
**synthesise convincing budgie chirps and chatter**. "Convincing" here means a
budgie owner would look round to see which bird is in the room. It doesn't
just mean bird-ish.

This sits next to two existing cards and borrows from both:

- **44 Birds**: procedural songbird DDS with a Turing-style lock/change
  grammar. Its `HANDOVER.md` covers the RP2040 audio-callback lessons (fixed
  point, click-free envelopes, ISR budget). Read it first.
- **105 Voder**: formant filters and a vowel space. It's useful for the
  talking/mimicry layer described below.

Budgies are a different problem from songbirds. They are open-ended vocal
learners. Their chatter (the *warble*) has no fixed song: it's a continuous,
recombining stream of elements drawn from a learned repertoire, with real
sequential grammar and flock dialects. That makes them a good fit for a card
about *language* rather than a card about *a song*.

---

## 1. What the research says

The numbers below come from the literature. Values marked **(est.)** are
working estimates. Check them against the full papers or against our own
measurements of recordings before relying on them (see §5, Phase 0).

### 1.1 The vocal repertoire

| Vocalisation | Who / when | Acoustic character |
|---|---|---|
| **Contact call** | Both sexes; flock cohesion, "where are you?" | Narrowband, strongly frequency-modulated tone. Lasts **~100–150 ms**, with most energy at **2–4 kHz**. Each bird has its own learned signature, and flock-mates converge on shared versions. |
| **Warble** (chatter) | Mostly males, courtship and social contexts | Long (seconds to minutes), **low-amplitude**, highly variable stream of elements at about **3–4 elements/s**, with no fixed order but strong statistical structure. |
| **Alarm call** | Both sexes | Harsh, broadband, noisy. |
| **Soft calls / begging** | Close-range; nestlings | Quiet. A fledgling's first contact call resembles a shortened version of its food-begging call. |
| **Learned speech / mimicry** | Pet birds, mostly males | Built from the same machinery. Amplitude modulation is prominent in both natural contact calls and learned English words. |

### 1.2 The "phonetics": warble element categories

- Farabaugh, Brown & Dooling (1992) sorted about 2,800 warble syllables into
  **42 classes**: 15 elemental and 27 compound (two or more elementals joined).
  The elementals fall into three families: **narrowband** (contact-call-like),
  **non-harmonic broadband** (alarm-like) and **harmonic broadband**.
- Tu, Smith & Dooling (2011) used a neural-network classifier on more than
  25,000 elements from 4 birds. They found **7 basic acoustic categories plus 1
  compound category**, in similar proportions across individuals. Budgies
  *perceive* these as categories too: they discriminate between categories
  better than within them.
- Madabhushi et al. (2023, *J Exp Biol*) use a closely matching set of 7 note
  types. This is the set the card will use:

| # | Category | Synthesis recipe (proposed) |
|---|---|---|
| A | **Alarm-call-like** | Band-limited noise plus a rough tonal component; fast AM; harsh |
| B | **Contact-call-like** | One FM tone, 2–4 kHz, ~100–150 ms, with a characteristic multi-inflection contour and AM |
| C | **Long harmonic** (>100 ms) | Harmonic stack on a lower f0 (est. 0.5–1.5 kHz) with a slow pitch contour |
| D | **Short harmonic** (<100 ms) | Same as C, short and often with a steep sweep |
| E | **Noisy** | Broadband noise burst with a spectral tilt and a coloured band |
| F | **Click** | Very short broadband impulse (est. 1–10 ms), often in trains |
| G | **Soft call** | Low-amplitude tonal call, a quieter and simpler version of B |
| + | **Compound** | Concatenations of the above with no gap |

### 1.3 The "phonology": segments inside a syllable

Mann, Fitch, Tu & Hoeschele (2021, *Sci Rep*) analysed warble at the level of
**segments** (below the breath-delimited syllable). They found, in four
independent populations:

- Songs are built from **consonant-like** segments (noisy/click/transient) and
  **vowel-like** segments (tonal/harmonic).
- Segments are **not randomly ordered**. As in human speech, syllables tend to
  **start with consonant-like segments**, and the **final segment tends to be
  longer, quieter and lower in f0**.

This gives the card a simple, testable **phonotactic rule** ("onset
transient, then tonal nucleus, then softer, lower, longer coda"). It's cheap to
apply, and it probably accounts for much of the difference between real budgie
chatter and random bleeps.

### 1.4 The "syntax": how elements are sequenced

- Warble sequences are **non-random, with at least 5th-order Markov
  structure** (Tu & Dooling). A first-order transition table will sound too
  random.
- Budgies notice when natural warble order is violated. Out-of-order elements
  inserted into a stream were detected *only* by budgies, not by other species
  tested (Tu & Dooling 2012). So the order matters to the birds, and a
  convincing synth needs it.
- **Dialects:** two colonies had colony-specific repetitive sequence patterns.
  When the colonies were mixed, their syntax **converged** and the old patterns
  disappeared (Madabhushi et al. 2023).
- **Rhythm:** van der Aa, Koliander, Fitch & Hoeschele (2025) found non-random,
  **music-like rhythmic structure** in inter-onset-interval ratios. It is
  driven by **two specific element pairs**, and individual males use them in
  different ways.
- Warble syntax is similar whether directed at males or females. It becomes
  *more similar between individuals* when directed at females.

### 1.5 The "pragmatics": social behaviour

- **Contact-call convergence:** birds housed together converge on shared
  contact calls over a period of weeks. Juveniles copy parents and flock-mates
  (Brittan-Powell et al. 1997; Farabaugh et al. 1994).
- **Auditory feedback:** budgies shift call pitch to compensate for
  pitch-shifted feedback. They show a **Lombard effect** (they get louder in
  noise) and a **Fletcher effect** (Osmanski & Dooling).
- Birds trained with rewards changed the intensity and spectro-temporal shape
  of their calls within days (Manabe & Dooling).

### 1.6 The instrument: how the sound is made

- Contact calls behave like a **single narrowband source**. Heliox experiments
  shifted the spectrum only slightly, so the trachea plays only a minor
  filtering role (Brittan-Powell et al. 1997). In practice, **contact calls
  don't need a formant filter**: a well-contoured FM/AM oscillator is the right
  model, and it's cheap.
- **Amplitude modulation** is a defining feature of budgie phonation (Banta
  Lavenex 1999). It's present in contact calls and in learned speech. A plain
  FM sine is missing this "buzz", and that's likely why many synthetic
  budgies sound like R2-D2.
- Parrots *can* use the tongue and tracheal resonance to shape vowels in
  speech mimicry. Model that with a movable resonant filter (see 105 Voder),
  used only in the mimicry layer.
- The 2–4 kHz call band sits in budgie hearing's most sensitive region, so
  there's a perceptual reason it matters.

### 1.7 Prior art in synthesis

- **Gutscher, Pucher, Lozo, Hoeschele & Mann (2019)**, *Statistical
  parametric synthesis of budgerigar songs*: they segmented recordings,
  clustered the elements and trained an HMM (HTS) synthesiser. Listeners rated
  synthesised, re-synthesised and natural budgie song **as not significantly
  different in naturalness**. Parametric, element-based synthesis is
  therefore proven to work for budgies. Our version is a lightweight embedded
  form of the same idea.
- **Hunter Adams' RP2040 birdsong lab**: spectrogram-traced DDS on this exact
  chip (also the basis of 44 Birds).

---

## 2. Card concept

> **Budgie** is a parametric budgerigar voice driven by a small model of budgie
> language: *phonetics* (7 element types), *phonotactics* (consonant → vowel →
> soft coda), *syntax* (high-order sequence memory with rhythm), and
> *pragmatics* (two birds that answer, converge, and get louder when you make
> noise).

The panel exposes the levels of the model one at a time, so you can study the
language as well as play it.

Two boot modes, following the 105 Voder convention:

| Boot | Mode | Purpose |
|---|---|---|
| Normal | **Aviary** | Generative. Two birds chatter, call to each other and react to the patch. |
| Hold Z down at power-on | **Phonetics Lab** | Study mode. Choose one element category, trigger it, sweep its parameters and hear what each dimension does. |

### 2.1 Panel: Aviary mode

| Control | Function |
|---|---|
| **Main knob** | **Mood / arousal.** CCW: drowsy soft calls and sparse warble. Centre: relaxed warble with occasional contact calls. CW: excited fast chatter, more contact calls, then alarm bursts at full. This moves category weights, tempo, loudness and noisiness together. |
| **X knob** | **Individual.** Moves through a space of bird identities (signature contact-call contour, f0 range, AM rate, preferred motifs). Each position is a repeatable bird. |
| **Y knob** | **Chatter density / tempo.** Centre ≈ natural 3–4 elements/s. CCW gives longer pauses and bout structure; CW gives breathless runs. |
| **Z up** | **Flock**: two birds converse on Out 1 and Out 2, with call-and-answer, overlapping warble, and slow **convergence** of their calls and syntax. |
| **Z middle** | **Solo**: one bird on both outputs (Out 2 is a "cage" version: band-limited, quieter, slightly delayed). |
| **Z down (momentary)** | **Call**: a tap makes the bird(s) answer with a contact call. Hold to freeze the current motif and loop it, like the lock on 44 Birds. |
| **Audio In 1** | **Listen.** Its envelope drives the Lombard / social response: sound in the room makes the birds louder and chattier, and a sudden loud transient can trigger an alarm. |
| **Audio In 2** | **Mimic source** (Phase 4). A pitch and envelope tracker turns incoming audio into a new learned element. |
| **CV In 1** | Pitch offset (V/oct-ish, around the bird's natural register) |
| **CV In 2** | Mood CV (sums with Main) |
| **Pulse In 1** | Trigger a contact call (answer) |
| **Pulse In 2** | Element clock: when patched, each pulse advances one element, for budgie in time with the patch |
| **Audio Out 1 / 2** | Bird A / Bird B |
| **CV Out 1** | Pitch contour of Bird A (held during silence) |
| **CV Out 2** | Element-category step CV (7 levels), or Bird A's amplitude envelope (option) |
| **Pulse Out 1** | Element onsets (Bird A) |
| **Pulse Out 2** | Contact-call and alarm events (either bird) |
| **LEDs** | Current element category (6 LEDs = A–F; soft call G = dim/all), brightness follows amplitude |

### 2.2 Panel: Phonetics Lab mode

| Control | Function |
|---|---|
| **Main** | Select category A–G (and Compound) |
| **X** | Variant within category, moving through stored real contours |
| **Y** | The "most important" dimension for that category: FM depth for B, harmonicity for C/D, noise colour for E, train rate for F |
| **Z up / middle** | Single element / phonotactic syllable (onset + nucleus + coda) |
| **Z down / Pulse In 1** | Trigger |
| **CV In 1/2** | Pitch / duration scaling |
| **Outputs** | As in Aviary. The CV outs make it easy to scope the contours. |

This mode is the "explore the language" part of the brief. It's also the
fastest way to tune each element against real recordings.

---

## 3. Synthesis engine

Each bird is one **syrinx voice**, computed per sample at 48 kHz:

```
            ┌─ tonal source: phase acc → sine LUT, + 1–4 harmonics (harmonicity)
 f0(t) ────►│      × AM: (1 − d + d·sin(2π·fam·t))      ← the budgie "buzz"
            └─ noise source: LFSR → 1-pole tilt → SVF bandpass (centre, Q)
                       │                   │
                    mix(t)  ───────────────┘
                       │
                  amp env(t) ──► [optional formant SVF pair, mimic layer only] ──► out
```

- **Contours, not oscillators.** Each element is a small parameter record:
  duration; f0 as 3–5 breakpoints with curvature (start / inflections / end);
  AM rate and depth; harmonicity; noise mix, colour and bandwidth; attack and
  decay; amplitude. A control-rate interpolator (e.g. 1–2 kHz) updates the
  per-sample increments.
- **Click trains** (F) are short windowed impulses into the noise filter, so no
  oscillator is involved.
- **Cost estimate:** two voices × (phase accumulator + up to 4 LUT reads + LFSR
  + one SVF + AM multiply) is far inside the ~3,000-cycle per-sample budget at
  144 MHz. The grammar runs at control rate or on core 1 (see the ComputerCard
  `second_core` example).
- **Anti-aliasing:** f0 up to about 5 kHz with 4 harmonics stays below 24 kHz
  Nyquist. Limit the harmonic count by f0.
- Use 44 Birds' click-free envelope and smoothing approach.

## 4. Language model

1. **Element inventory:** about 16–64 stored element templates per category,
   extracted from real recordings (Phase 0). Each is a few dozen bytes, so
   thousands fit easily in flash.
2. **Bird identity:** a per-bird random but repeatable transform of the
   templates: f0 scale, AM rate, contour warp, and a personal **signature
   contact call**. This is selected by X.
3. **Phonotactics:** a syllable is optional consonant-like onset (F/E or a
   noisy attack) + tonal nucleus (B/C/D) + optional coda that is **longer,
   quieter and lower in f0** (Mann et al. 2021).
4. **Syntax:** a **variable-order Markov model** (backs off from order 5 down
   to order 1) over categories, trained offline from recordings. Cheap
   approximations for embedded use:
   - a first-order category table plus a **motif memory**: a ring buffer of
     recent elements, with a probability of re-quoting a remembered run of 2–8
     elements, possibly varied. This gives the colony-specific "repetitive
     patterns" that real warble shows.
   - The Main/mood knob reweights rows of the table. It doesn't replace them.
5. **Rhythm:** inter-onset intervals are drawn from a learned IOI-ratio
   distribution, with the two key element pairs (van der Aa et al. 2025)
   getting their own timing rules. Y scales the tempo.
6. **Pragmatics:**
   - *Answering:* a contact call from one bird raises the other's chance of
     answering with its own contact call after about 200–600 ms (est.).
   - *Convergence:* in Flock mode, each answer moves the answering bird's
     signature call and motif memory slightly toward the other's. Real birds
     take weeks; here it happens over minutes. Changing X resets one bird to a
     new individual, which then has to converge again.
   - *Lombard / social facilitation:* the Audio In 1 envelope raises amplitude
     and chatter density.
   - *Startle:* a sharp transient on Audio In 1 triggers an alarm call A,
     followed by a short silence.

---

## 5. Roadmap

**Phase 0: Analysis toolchain (offline, Python). This is the key step for
"convincing".**

- Collect recordings: xeno-canto has many *Melopsittacus undulatus*
  recordings (check each one's licence; we export *parameters*, not samples).
  Our own recordings of pet budgies are also useful and avoid licence
  questions.
- Pipeline: band-pass → segment on energy → per-segment pitch track (YIN /
  pYIN), AM rate (envelope spectrum), harmonicity, spectral flatness,
  duration → cluster into the 7 categories (seeded by hand-labelled
  examples) → fit breakpoint contours → measure transition statistics and IOI
  ratios → **export a C header** of element templates and Markov tables.
- Check the "(est.)" numbers in §1 against our own measurements.
- Build a **resynthesis listening test**: play real clips next to
  analysed-then-resynthesised clips. If resynthesis isn't convincing, the
  generative layer won't be either.
- Place it in `tools/` or inside the card folder. Firmware gets data through
  generated headers, as 105 Voder does with `vowels.h`.

**Phase 1: Element synth plus Phonetics Lab mode.** Two voices, the full
element recipe set, and stored templates. Tune each category against Phase 0
references.

**Phase 2: Grammar.** Phonotactic syllables, the Markov/motif engine, rhythm,
and the mood mapping. This produces Aviary Solo mode.

**Phase 3: Flock.** Answering, convergence, Lombard/startle from Audio In 1,
and the CV/pulse outputs.

**Phase 4: Mimicry (stretch).** Pitch-track Audio In 2 into new elements that
are added to the bird's repertoire, and use the formant filter layer to make
it "talk" (reusing 105 Voder's filter code).

## 6. Risks and open questions

- **Getting the numbers.** The category definitions and timing figures in the
  primary papers (Farabaugh et al. 1992; Tu et al. 2011; Madabhushi et al.
  2023; van der Aa et al. 2025) should be read in full. This proposal was built
  from abstracts and summaries. Phase 0 measurements reduce this risk anyway.
- **5th-order syntax on a microcontroller.** Full 5th-order tables over 8
  symbols would be 8⁶ entries, mostly empty. A variable-order back-off model or
  the motif-memory approximation is sparse and small. Listening tests decide
  which to use.
- **AM is probably the main ingredient for realism.** Prototype it first in
  Phase 1.
- **Card number:** to be claimed via PR when firmware work starts.

## 7. Sources

- Farabaugh, Brown & Dooling (1992). Analysis of warble song of the
  budgerigar. *Bioacoustics*.
  <https://bioacoustics.info/article/analysis-warble-song-budgerigar-melopsittacus-undulatus>
- Tu, H.-W. (2011). *The structure and perception of budgerigar warble songs*
  (PhD, Univ. Maryland). <https://drum.lib.umd.edu/handle/1903/10031>
- Tu, Smith & Dooling (2011). Acoustic and perceptual categories of vocal
  elements in the warble song of budgerigars. *J Comp Psychol*.
  <https://pmc.ncbi.nlm.nih.gov/articles/PMC4497543>
- Tu, Osmanski & Dooling (2011). Learned vocalizations in budgerigars. *JASA*.
  <https://pmc.ncbi.nlm.nih.gov/articles/PMC3087398/>
- Madabhushi et al. (2023). Higher-order dialectic variation and syntactic
  convergence in the complex warble song of budgerigars. *J Exp Biol* 226.
  <https://journals.biologists.com/jeb/article/226/20/jeb245678/334187/Higher-order-dialectic-variation-and-syntactic>
- Mann, Fitch, Tu & Hoeschele (2021). Universal principles underlying
  segmental structures in parrot song and human speech. *Sci Rep*.
  <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7804275/>
- van der Aa, Koliander, Fitch & Hoeschele (2025). Novel approach to
  inter-onset-interval ratio uncovers music-like rhythmic patterns in
  budgerigar warble song. *Ann NY Acad Sci*.
  <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7618608/>
- Brittan-Powell et al. (1997). Mechanisms of vocal production in
  budgerigars. *JASA*.
  <https://pubmed.ncbi.nlm.nih.gov/9000746>
- Banta Lavenex (1999). Vocal production mechanisms in the budgerigar: the
  presence and implications of amplitude modulation. *JASA* 106(1).
  <https://pubs.aip.org/asa/jasa/article-abstract/106/1/491/553262/>
- Gutscher, Pucher, Lozo, Hoeschele & Mann (2019). Statistical parametric
  synthesis of budgerigar songs. <https://bio.acousti.ca/uk/node/57584>
- Does audience affect the structure of warble song in budgerigars? *Behav Processes*.
  <https://pmc.ncbi.nlm.nih.gov/articles/PMC5906206>
- Osmanski & Dooling: altered auditory feedback in budgerigars.
  <https://www.researchgate.net/publication/26702930>
- Farabaugh et al.: vocal development in budgerigars, contact calls.
  <https://www.researchgate.net/profile/Susan-Farabaugh/publication/13935059>
- Hunter Adams: RP2040 birdsong synthesis.
  <https://vanhunteradams.com/Pico/Birds/Birdsong.html>
