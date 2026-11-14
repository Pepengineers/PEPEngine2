#pragma once
enum class MetaValidationMatrix { None, AO_SSR, FSR };
int RunMetaApplication(bool smoke, bool focused = false, bool gbv = false, bool sceneOnly = false,
    MetaValidationMatrix matrix = MetaValidationMatrix::None, unsigned firstCase = 0);
