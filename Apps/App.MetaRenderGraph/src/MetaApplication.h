#pragma once
enum class MetaValidationMatrix { None, AO_SSR, FSR, ParticleInteraction };
struct MetaBenchmark {
    unsigned Warmup = 120, Frames = 0;
    bool Direct = false, Serial = false, FSR = false, Trace = false, Proof = false;
};
int RunMetaApplication(bool smoke, bool focused = false, bool gbv = false, bool sceneOnly = false,
    MetaValidationMatrix matrix = MetaValidationMatrix::None, unsigned firstCase = 0, MetaBenchmark benchmark = {});
