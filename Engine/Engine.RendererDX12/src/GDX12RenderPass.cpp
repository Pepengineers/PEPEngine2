#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <stdexcept>
#include <typeinfo>

void GDX12RenderPass::SetInputs(std::vector<IRenderPassLink*> inputs)
{
    if (inputs.size() < _numInputs)
        throw std::invalid_argument(std::string(typeid(*this).name()) + ": expected at least " +
            std::to_string(_numInputs) + " inputs, got " + std::to_string(inputs.size()));
    for (size_t i = 0; i < inputs.size(); ++i)
        if (!inputs[i])
            throw std::invalid_argument(std::string(typeid(*this).name()) + ": null input " + std::to_string(i));
    _inputs = std::move(inputs);
}
