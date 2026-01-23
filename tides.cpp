// Tides 2 for Disting NT - v2.0.0 COMPLETE REWRITE
// Clean implementation based on MI Tides 2 behavior
// Original concept: Emilie Gillet / Mutable Instruments
// Disting NT implementation: Claude / Anthropic

#include <math.h>
#include <new>
#include <cstring>
#include <distingnt/api.h>

// ============================================================================
// Constants
// ============================================================================

static constexpr float PI = 3.14159265358979323846f;
static constexpr float TWO_PI = 2.0f * PI;

// Base frequencies for the three ranges (Hz)
static constexpr float FREQ_LOW = 0.125f;      // 1/8 Hz - very slow LFO
static constexpr float FREQ_MEDIUM = 2.0f;      // 2 Hz - standard LFO
static constexpr float FREQ_HIGH = 130.81f;     // C3 - audio rate

// ============================================================================
// Enums
// ============================================================================

enum RampMode {
    RAMP_AD = 0,      // Attack/Decay envelope (one-shot, triggered)
    RAMP_CYCLE = 1,   // Free-running LFO/VCO
    RAMP_AR = 2       // Attack/Release (gate-following)
};

enum FreqRange {
    RANGE_LOW = 0,
    RANGE_MEDIUM = 1,
    RANGE_HIGH = 2
};

enum OutputMode {
    OUT_GATES = 0,        // Main + Raw + EOA + EOR
    OUT_AMPLITUDE = 1,    // Panning/crossfade across 4 outputs
    OUT_SLOPE_PHASE = 2,  // 4 phase-shifted copies
    OUT_FREQUENCY = 3     // Polyrhythmic divisions
};

// ============================================================================
// DTC Memory (fast memory for real-time DSP)
// ============================================================================

struct _TidesDTC {
    // Phase accumulators (0.0 to 1.0)
    float phase[4];
    
    // Gate state
    bool gate_high;
    bool prev_gate_high;
    
    // Envelope state for AD/AR modes
    bool envelope_running;
    float envelope_phase;
    
    // Smoothed parameters (for zipper-free changes)
    float smooth_freq;
    float smooth_shape;
    float smooth_slope;
    float smooth_smoothness;
    float smooth_shift;
};

// ============================================================================
// Algorithm Structure
// ============================================================================

struct _TidesAlgorithm : public _NT_algorithm {
    _TidesAlgorithm(_TidesDTC* dtc_) : dtc(dtc_) {}
    ~_TidesAlgorithm() {}
    
    _TidesDTC* dtc;
    float inv_sample_rate;
};

// ============================================================================
// Parameters
// ============================================================================

enum {
    // Inputs (Page 1)
    kParam_TrigInput,
    kParam_VOctInput,
    kParam_FMInput,
    kParam_ShapeInput,
    kParam_SlopeInput,
    kParam_SmoothInput,
    kParam_ShiftInput,
    
    // Outputs (Page 2) - default to buses 13-16
    kParam_Output1,
    kParam_Output1Mode,
    kParam_Output2,
    kParam_Output2Mode,
    kParam_Output3,
    kParam_Output3Mode,
    kParam_Output4,
    kParam_Output4Mode,
    
    // Mode (Page 3)
    kParam_RampMode,
    kParam_Range,
    kParam_OutputMode,
    
    // Main (Page 4)
    kParam_Frequency,
    kParam_Shape,
    kParam_Slope,
    kParam_Smoothness,
    kParam_Shift,
    
    // Modulation (Page 5)
    kParam_FMAmount,
    kParam_ShapeAtten,
    kParam_SlopeAtten,
    kParam_SmoothAtten,
    kParam_ShiftAtten,
    
    kNumParams
};

static const char* rampModeNames[] = { "AD", "Cycle", "AR", NULL };
static const char* rangeNames[] = { "Low", "Medium", "High", NULL };
static const char* outputModeNames[] = { "Gates", "Amplitude", "Slope/Phase", "Frequency", NULL };

static const _NT_parameter parameters[] = {
    // Inputs - page 1
    NT_PARAMETER_CV_INPUT("Trig/Gate In", 0, 0)
    NT_PARAMETER_CV_INPUT("V/Oct In", 0, 0)
    NT_PARAMETER_CV_INPUT("FM In", 0, 0)
    NT_PARAMETER_CV_INPUT("Shape In", 0, 0)
    NT_PARAMETER_CV_INPUT("Slope In", 0, 0)
    NT_PARAMETER_CV_INPUT("Smooth In", 0, 0)
    NT_PARAMETER_CV_INPUT("Shift In", 0, 0)
    
    // Outputs - page 2 (default to buses 13, 14, 15, 16)
    NT_PARAMETER_CV_OUTPUT_WITH_MODE("Output 1", 1, 13)
    NT_PARAMETER_CV_OUTPUT_WITH_MODE("Output 2", 1, 14)
    NT_PARAMETER_CV_OUTPUT_WITH_MODE("Output 3", 1, 15)
    NT_PARAMETER_CV_OUTPUT_WITH_MODE("Output 4", 1, 16)
    
    // Mode - page 3
    { .name = "Ramp Mode", .min = 0, .max = 2, .def = RAMP_CYCLE, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = rampModeNames },
    { .name = "Range", .min = 0, .max = 2, .def = RANGE_MEDIUM, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = rangeNames },
    { .name = "Output Mode", .min = 0, .max = 3, .def = OUT_SLOPE_PHASE, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = outputModeNames },
    
    // Main parameters - page 4
    // Frequency: ±5 octaves from base frequency
    { .name = "Frequency", .min = -60, .max = 60, .def = 0, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },
    // Shape: 0-100%, controls waveshaping (expo ↔ linear ↔ log)
    { .name = "Shape", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    // Slope: 0-100%, controls attack/decay ratio
    { .name = "Slope", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    // Smoothness: 0-100%, <50% = lowpass, >50% = wavefold
    { .name = "Smoothness", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    // Shift: 0-100%, function depends on output mode
    { .name = "Shift", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    
    // Modulation attenuverters - page 5
    { .name = "FM Amount", .min = -100, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Shape Atten", .min = -100, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Slope Atten", .min = -100, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Smooth Atten", .min = -100, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Shift Atten", .min = -100, .max = 100, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
};

// Page definitions
static const uint8_t pageInputs[] = { 
    kParam_TrigInput, kParam_VOctInput, kParam_FMInput, 
    kParam_ShapeInput, kParam_SlopeInput, kParam_SmoothInput, kParam_ShiftInput 
};
static const uint8_t pageOutputs[] = {
    kParam_Output1, kParam_Output1Mode, kParam_Output2, kParam_Output2Mode,
    kParam_Output3, kParam_Output3Mode, kParam_Output4, kParam_Output4Mode
};
static const uint8_t pageMode[] = {
    kParam_RampMode, kParam_Range, kParam_OutputMode
};
static const uint8_t pageMain[] = {
    kParam_Frequency, kParam_Shape, kParam_Slope, kParam_Smoothness, kParam_Shift
};
static const uint8_t pageMod[] = {
    kParam_FMAmount, kParam_ShapeAtten, kParam_SlopeAtten, kParam_SmoothAtten, kParam_ShiftAtten
};

static const _NT_parameterPage pages[] = {
    { .name = "Inputs", .numParams = sizeof(pageInputs), .params = pageInputs },
    { .name = "Outputs", .numParams = sizeof(pageOutputs), .params = pageOutputs },
    { .name = "Mode", .numParams = sizeof(pageMode), .params = pageMode },
    { .name = "Main", .numParams = sizeof(pageMain), .params = pageMain },
    { .name = "Modulation", .numParams = sizeof(pageMod), .params = pageMod },
};

static const _NT_parameterPages parameterPages = {
    .numPages = sizeof(pages) / sizeof(pages[0]),
    .pages = pages,
};

// ============================================================================
// DSP Helper Functions
// ============================================================================

// Convert semitones to frequency ratio
static inline float semitonesToRatio(float semitones) {
    return expf(semitones * 0.0577622650f);  // ln(2)/12
}

// Clamp value to range
static inline float clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

// One-pole lowpass for parameter smoothing
static inline float smoothParam(float current, float target, float coeff) {
    return current + coeff * (target - current);
}

// Apply asymmetric slope to a ramp (0-1 input → 0-1 output with variable rise/fall)
static inline float applySlope(float phase, float slope) {
    // slope = 0: all rise, no fall
    // slope = 0.5: symmetric triangle
    // slope = 1: no rise, all fall
    
    float pw = clamp(slope, 0.001f, 0.999f);  // Pulse width (rise portion)
    
    if (phase < pw) {
        // Rising phase
        return phase / pw;
    } else {
        // Falling phase
        return 1.0f - (phase - pw) / (1.0f - pw);
    }
}

// Apply waveshaping based on Shape parameter
// shape = 0: exponential (slow start, fast end)
// shape = 0.5: linear
// shape = 1: logarithmic (fast start, slow end)
static inline float applyShape(float x, float shape) {
    if (shape < 0.5f) {
        // Exponential curve
        float amount = 1.0f - shape * 2.0f;  // 1 at shape=0, 0 at shape=0.5
        float curved = x * x * x;  // Cubic for expo feel
        return x + amount * (curved - x);
    } else {
        // Logarithmic curve
        float amount = (shape - 0.5f) * 2.0f;  // 0 at shape=0.5, 1 at shape=1
        float curved = 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
        return x + amount * (curved - x);
    }
}

// Apply smoothness: <50% = lowpass filter, >50% = wavefold
static inline float applySmoothness(float x, float smoothness, float& lpState) {
    if (smoothness < 0.5f) {
        // Lowpass filtering
        float cutoff = smoothness * 2.0f;  // 0 = no filtering, 1 = full filtering
        float coeff = 0.01f + cutoff * 0.49f;  // Smoothing coefficient
        lpState = lpState + coeff * (x - lpState);
        return lpState;
    } else {
        // Wavefolding
        float foldAmount = (smoothness - 0.5f) * 2.0f;  // 0 at 50%, 1 at 100%
        float folded = x;
        
        // Simple triangle wavefolder
        if (foldAmount > 0.0f) {
            float gain = 1.0f + foldAmount * 3.0f;  // Amplify before folding
            folded = x * gain;
            
            // Fold the signal
            while (folded > 1.0f || folded < -1.0f) {
                if (folded > 1.0f) folded = 2.0f - folded;
                if (folded < -1.0f) folded = -2.0f - folded;
            }
        }
        
        return folded;
    }
}

// ============================================================================
// Plugin Callbacks
// ============================================================================

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* /* specifications */) {
    req.numParameters = kNumParams;
    req.sram = sizeof(_TidesAlgorithm);
    req.dram = 0;
    req.dtc = sizeof(_TidesDTC);
    req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& /* req */, const int32_t* /* specifications */) {
    _TidesDTC* dtc = (_TidesDTC*)ptrs.dtc;
    _TidesAlgorithm* alg = new (ptrs.sram) _TidesAlgorithm(dtc);
    
    alg->parameters = parameters;
    alg->parameterPages = &parameterPages;
    alg->inv_sample_rate = 1.0f / NT_globals.sampleRate;
    
    // Initialize DTC
    memset(dtc, 0, sizeof(_TidesDTC));
    dtc->smooth_shape = 0.5f;
    dtc->smooth_slope = 0.5f;
    dtc->smooth_smoothness = 0.5f;
    dtc->smooth_shift = 0.5f;
    
    return alg;
}

void parameterChanged(_NT_algorithm* /* self */, int /* p */) {
    // Parameters are read directly in step(), smoothing applied there
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    _TidesAlgorithm* alg = (_TidesAlgorithm*)self;
    _TidesDTC* dtc = alg->dtc;
    int numFrames = numFramesBy4 * 4;
    
    // === Read parameters ===
    const RampMode rampMode = (RampMode)alg->v[kParam_RampMode];
    const FreqRange range = (FreqRange)alg->v[kParam_Range];
    const OutputMode outputMode = (OutputMode)alg->v[kParam_OutputMode];
    
    // Get base frequency for current range
    float baseFreq;
    switch (range) {
        case RANGE_LOW:    baseFreq = FREQ_LOW; break;
        case RANGE_MEDIUM: baseFreq = FREQ_MEDIUM; break;
        case RANGE_HIGH:   baseFreq = FREQ_HIGH; break;
        default:           baseFreq = FREQ_MEDIUM; break;
    }
    
    // Frequency with semitone offset
    float freqSemitones = (float)alg->v[kParam_Frequency];
    float frequency = baseFreq * semitonesToRatio(freqSemitones);
    
    // Normalized parameters (0-1)
    float targetShape = alg->v[kParam_Shape] / 100.0f;
    float targetSlope = alg->v[kParam_Slope] / 100.0f;
    float targetSmoothness = alg->v[kParam_Smoothness] / 100.0f;
    float targetShift = alg->v[kParam_Shift] / 100.0f;
    
    // Attenuverters
    float fmAtten = alg->v[kParam_FMAmount] / 100.0f;
    float shapeAtten = alg->v[kParam_ShapeAtten] / 100.0f;
    float slopeAtten = alg->v[kParam_SlopeAtten] / 100.0f;
    float smoothAtten = alg->v[kParam_SmoothAtten] / 100.0f;
    float shiftAtten = alg->v[kParam_ShiftAtten] / 100.0f;
    
    // === Get CV inputs ===
    const int trigBus = alg->v[kParam_TrigInput];
    const int voctBus = alg->v[kParam_VOctInput];
    const int fmBus = alg->v[kParam_FMInput];
    const int shapeBus = alg->v[kParam_ShapeInput];
    const int slopeBus = alg->v[kParam_SlopeInput];
    const int smoothBus = alg->v[kParam_SmoothInput];
    const int shiftBus = alg->v[kParam_ShiftInput];
    
    const float* trigIn = (trigBus > 0) ? busFrames + (trigBus - 1) * numFrames : nullptr;
    const float* voctIn = (voctBus > 0) ? busFrames + (voctBus - 1) * numFrames : nullptr;
    const float* fmIn = (fmBus > 0) ? busFrames + (fmBus - 1) * numFrames : nullptr;
    const float* shapeIn = (shapeBus > 0) ? busFrames + (shapeBus - 1) * numFrames : nullptr;
    const float* slopeIn = (slopeBus > 0) ? busFrames + (slopeBus - 1) * numFrames : nullptr;
    const float* smoothIn = (smoothBus > 0) ? busFrames + (smoothBus - 1) * numFrames : nullptr;
    const float* shiftIn = (shiftBus > 0) ? busFrames + (shiftBus - 1) * numFrames : nullptr;
    
    // === Get output buses ===
    const int out1Bus = alg->v[kParam_Output1];
    const int out2Bus = alg->v[kParam_Output2];
    const int out3Bus = alg->v[kParam_Output3];
    const int out4Bus = alg->v[kParam_Output4];
    
    float* out1 = (out1Bus > 0) ? busFrames + (out1Bus - 1) * numFrames : nullptr;
    float* out2 = (out2Bus > 0) ? busFrames + (out2Bus - 1) * numFrames : nullptr;
    float* out3 = (out3Bus > 0) ? busFrames + (out3Bus - 1) * numFrames : nullptr;
    float* out4 = (out4Bus > 0) ? busFrames + (out4Bus - 1) * numFrames : nullptr;
    
    const bool out1Replace = alg->v[kParam_Output1Mode];
    const bool out2Replace = alg->v[kParam_Output2Mode];
    const bool out3Replace = alg->v[kParam_Output3Mode];
    const bool out4Replace = alg->v[kParam_Output4Mode];
    
    // Smoothing coefficient (about 5ms)
    const float smoothCoeff = 0.005f;
    
    // Lowpass state for smoothness processing
    static float lpState[4] = {0, 0, 0, 0};
    
    // === Process each sample ===
    for (int i = 0; i < numFrames; ++i) {
        // --- Apply CV modulation ---
        float cvFreq = frequency;
        if (voctIn) {
            // V/Oct: 5V = 1.0 in Disting NT = 5 octaves
            cvFreq *= semitonesToRatio(voctIn[i] * 12.0f);
        }
        if (fmIn) {
            cvFreq *= semitonesToRatio(fmIn[i] * 12.0f * fmAtten);
        }
        
        // Smooth and modulate other parameters
        dtc->smooth_shape = smoothParam(dtc->smooth_shape, targetShape, smoothCoeff);
        dtc->smooth_slope = smoothParam(dtc->smooth_slope, targetSlope, smoothCoeff);
        dtc->smooth_smoothness = smoothParam(dtc->smooth_smoothness, targetSmoothness, smoothCoeff);
        dtc->smooth_shift = smoothParam(dtc->smooth_shift, targetShift, smoothCoeff);
        
        float shape = dtc->smooth_shape;
        float slope = dtc->smooth_slope;
        float smoothness = dtc->smooth_smoothness;
        float shift = dtc->smooth_shift;
        
        if (shapeIn) shape = clamp(shape + shapeIn[i] * 0.1f * shapeAtten, 0.0f, 1.0f);
        if (slopeIn) slope = clamp(slope + slopeIn[i] * 0.1f * slopeAtten, 0.0f, 1.0f);
        if (smoothIn) smoothness = clamp(smoothness + smoothIn[i] * 0.1f * smoothAtten, 0.0f, 1.0f);
        if (shiftIn) shift = clamp(shift + shiftIn[i] * 0.1f * shiftAtten, 0.0f, 1.0f);
        
        // --- Handle gate/trigger ---
        bool gate = false;
        bool rising = false;
        
        if (trigIn) {
            gate = trigIn[i] > 1.0f;  // ~1V threshold
            rising = gate && !dtc->prev_gate_high;
            dtc->prev_gate_high = gate;
        }
        
        // --- Update phase based on ramp mode ---
        float phaseInc = cvFreq * alg->inv_sample_rate;
        
        switch (rampMode) {
            case RAMP_AD:
                // Attack/Decay: trigger starts envelope, runs once to completion
                if (rising) {
                    dtc->phase[0] = 0.0f;
                    dtc->envelope_running = true;
                }
                if (dtc->envelope_running) {
                    dtc->phase[0] += phaseInc;
                    if (dtc->phase[0] >= 1.0f) {
                        dtc->phase[0] = 1.0f;
                        dtc->envelope_running = false;
                    }
                }
                break;
                
            case RAMP_CYCLE:
                // Cyclic: free-running, trigger resets phase
                if (rising) {
                    dtc->phase[0] = 0.0f;
                }
                dtc->phase[0] += phaseInc;
                while (dtc->phase[0] >= 1.0f) {
                    dtc->phase[0] -= 1.0f;
                }
                break;
                
            case RAMP_AR:
                // Attack/Release: gate high = rise, gate low = fall
                if (gate || trigIn == nullptr) {
                    // Rising (or free-run if no gate connected)
                    if (trigIn == nullptr) {
                        // No gate = free-run like cycle mode
                        dtc->phase[0] += phaseInc;
                        while (dtc->phase[0] >= 1.0f) {
                            dtc->phase[0] -= 1.0f;
                        }
                    } else {
                        // Gate high = attack phase
                        float attackSpeed = phaseInc / clamp(slope, 0.01f, 0.99f);
                        dtc->phase[0] += attackSpeed;
                        if (dtc->phase[0] > 0.5f) dtc->phase[0] = 0.5f;  // Stop at apex
                    }
                } else {
                    // Gate low = release phase
                    float releaseSpeed = phaseInc / clamp(1.0f - slope, 0.01f, 0.99f);
                    dtc->phase[0] += releaseSpeed;
                    if (dtc->phase[0] > 1.0f) dtc->phase[0] = 1.0f;  // Stop at end
                }
                break;
        }
        
        // --- Generate raw ramp and shaped output ---
        float rawPhase = dtc->phase[0];
        float ramp;
        
        if (rampMode == RAMP_AR) {
            // AR mode: phase 0-0.5 = attack, 0.5-1.0 = release
            if (rawPhase <= 0.5f) {
                ramp = rawPhase * 2.0f;  // 0→1 during attack
            } else {
                ramp = 1.0f - (rawPhase - 0.5f) * 2.0f;  // 1→0 during release
            }
        } else {
            // AD and Cycle modes: apply slope to create asymmetric triangle
            ramp = applySlope(rawPhase, slope);
        }
        
        // Apply shape (waveshaping)
        float shaped = applyShape(ramp, shape);
        
        // Apply smoothness (lowpass or wavefold)
        float processed = applySmoothness(shaped, smoothness, lpState[0]);
        
        // Scale to ±5V for bipolar output (Cycle mode) or 0-8V unipolar (AD/AR)
        float out1Val, out2Val, out3Val, out4Val;
        
        // --- Generate outputs based on output mode ---
        switch (outputMode) {
            case OUT_GATES: {
                // Gates mode: 
                // Out1: Main shaped signal × shift level
                // Out2: Raw triangle (unshifted)
                // Out3: End of Attack gate (high when past attack portion)
                // Out4: End of Release/Ramp gate (high at end)
                
                float level = shift * 2.0f - 1.0f;  // Convert 0-1 to -1 to +1 for attenuverter
                
                if (rampMode == RAMP_CYCLE) {
                    // Bipolar output for cycle mode
                    out1Val = (processed * 2.0f - 1.0f) * 5.0f * fabsf(level);
                    if (level < 0.0f) out1Val = -out1Val;
                    out2Val = (ramp * 2.0f - 1.0f) * 5.0f;  // Raw bipolar ramp
                } else {
                    // Unipolar output for envelope modes
                    out1Val = processed * 8.0f * level;
                    out2Val = ramp * 8.0f;  // Raw unipolar ramp
                }
                
                // EOA: high when past attack portion
                bool pastAttack = (rampMode == RAMP_AR) ? (rawPhase >= 0.5f) : (rawPhase >= slope);
                out3Val = pastAttack ? 8.0f : 0.0f;
                
                // EOR: high at end of cycle/envelope
                bool atEnd = (rampMode == RAMP_CYCLE) ? (rawPhase < phaseInc * 2.0f) : (rawPhase >= 0.999f);
                out4Val = atEnd ? 8.0f : 0.0f;
                break;
            }
            
            case OUT_AMPLITUDE: {
                // Amplitude mode: signal panned across 4 outputs based on shift
                float signal;
                if (rampMode == RAMP_CYCLE) {
                    signal = (processed * 2.0f - 1.0f) * 5.0f;
                } else {
                    signal = processed * 8.0f;
                }
                
                // Shift controls which output(s) receive the signal
                // shift=0: all to out1, shift=1: all to out4
                float pos = shift * 3.0f;  // 0 to 3
                
                float gain1 = clamp(1.0f - pos, 0.0f, 1.0f);
                float gain2 = clamp(1.0f - fabsf(pos - 1.0f), 0.0f, 1.0f);
                float gain3 = clamp(1.0f - fabsf(pos - 2.0f), 0.0f, 1.0f);
                float gain4 = clamp(pos - 2.0f, 0.0f, 1.0f);
                
                out1Val = signal * gain1;
                out2Val = signal * gain2;
                out3Val = signal * gain3;
                out4Val = signal * gain4;
                break;
            }
            
            case OUT_SLOPE_PHASE: {
                // Slope/Phase mode: 4 phase-shifted copies
                float phaseSpread = shift;  // 0 = unison, 1 = 90° spread
                
                for (int ch = 0; ch < 4; ++ch) {
                    float phaseOffset = ch * phaseSpread * 0.25f;  // 0, 0.25, 0.5, 0.75 at shift=1
                    float p = rawPhase + phaseOffset;
                    while (p >= 1.0f) p -= 1.0f;
                    
                    float r = applySlope(p, slope);
                    float s = applyShape(r, shape);
                    float pr = applySmoothness(s, smoothness, lpState[ch]);
                    
                    float val;
                    if (rampMode == RAMP_CYCLE) {
                        val = (pr * 2.0f - 1.0f) * 5.0f;
                    } else {
                        val = pr * 8.0f;
                    }
                    
                    switch (ch) {
                        case 0: out1Val = val; break;
                        case 1: out2Val = val; break;
                        case 2: out3Val = val; break;
                        case 3: out4Val = val; break;
                    }
                }
                break;
            }
            
            case OUT_FREQUENCY: {
                // Frequency mode: 4 outputs at different frequency ratios
                // Shift selects the ratio set
                
                // Simple ratio sets (could be expanded)
                static const float ratios[4][4] = {
                    {1.0f, 0.5f, 0.25f, 0.125f},   // Octave divisions
                    {1.0f, 0.75f, 0.5f, 0.25f},    // Mixed
                    {1.0f, 0.667f, 0.5f, 0.333f},  // Thirds
                    {1.0f, 2.0f, 3.0f, 4.0f}       // Harmonics
                };
                
                int ratioSet = (int)(shift * 3.99f);
                ratioSet = clamp(ratioSet, 0, 3);
                
                // Update phase accumulators for each channel
                for (int ch = 0; ch < 4; ++ch) {
                    if (rising) {
                        dtc->phase[ch] = 0.0f;
                    }
                    
                    float chPhaseInc = phaseInc * ratios[ratioSet][ch];
                    dtc->phase[ch] += chPhaseInc;
                    while (dtc->phase[ch] >= 1.0f) {
                        dtc->phase[ch] -= 1.0f;
                    }
                    
                    float r = applySlope(dtc->phase[ch], slope);
                    float s = applyShape(r, shape);
                    float pr = applySmoothness(s, smoothness, lpState[ch]);
                    
                    float val;
                    if (rampMode == RAMP_CYCLE) {
                        val = (pr * 2.0f - 1.0f) * 5.0f;
                    } else {
                        val = pr * 8.0f;
                    }
                    
                    switch (ch) {
                        case 0: out1Val = val; break;
                        case 1: out2Val = val; break;
                        case 2: out3Val = val; break;
                        case 3: out4Val = val; break;
                    }
                }
                break;
            }
            
            default:
                out1Val = out2Val = out3Val = out4Val = 0.0f;
                break;
        }
        
        // --- Write to output buses ---
        if (out1) {
            if (out1Replace) out1[i] = out1Val;
            else out1[i] += out1Val;
        }
        if (out2) {
            if (out2Replace) out2[i] = out2Val;
            else out2[i] += out2Val;
        }
        if (out3) {
            if (out3Replace) out3[i] = out3Val;
            else out3[i] += out3Val;
        }
        if (out4) {
            if (out4Replace) out4[i] = out4Val;
            else out4[i] += out4Val;
        }
    }
}

// ============================================================================
// Factory Definition
// ============================================================================

static const _NT_factory factory = {
    .guid = NT_MULTICHAR('T', 'i', 'd', '2'),
    .name = "Tides 2",
    .description = "Tidal Modulator - LFO/Envelope/VCO",
    .numSpecifications = 0,
    .specifications = nullptr,
    .calculateStaticRequirements = nullptr,
    .initialise = nullptr,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = nullptr,
    .midiRealtime = nullptr,
    .midiMessage = nullptr,
    .tags = kNT_tagUtility,
    .hasCustomUi = nullptr,
    .customUi = nullptr,
    .setupUi = nullptr,
    .serialise = nullptr,
    .deserialise = nullptr,
    .midiSysEx = nullptr,
    .parameterUiPrefix = nullptr,
};

// ============================================================================
// Plugin Entry Point
// ============================================================================

uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
        case kNT_selector_version:
            return kNT_apiVersionCurrent;
        case kNT_selector_numFactories:
            return 1;
        case kNT_selector_factoryInfo:
            return (uintptr_t)((data == 0) ? &factory : nullptr);
    }
    return 0;
}
