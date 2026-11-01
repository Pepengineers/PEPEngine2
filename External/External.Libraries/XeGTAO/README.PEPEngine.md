# Intel XeGTAO

Official shader headers from [GameTechDev/XeGTAO](https://github.com/GameTechDev/XeGTAO/tree/a5b1686c7ea37788eeb3576b5be47f7c03db532c),
commit `a5b1686c7ea37788eeb3576b5be47f7c03db532c`. MIT license: `LICENSE`.
The step loop uses `[loop]` instead of `[unroll]` so its count can change at runtime.
The engine wrapper uses the official five-mip
depth prefilter, analytic horizon integration, Hilbert/R2 noise, edge output and
denoiser. FP32 depth avoids FP16 overflow with centimetre-scale scenes.
