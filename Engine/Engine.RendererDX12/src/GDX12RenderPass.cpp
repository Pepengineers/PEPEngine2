#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <stdexcept>
#include <typeinfo>

void GDX12RenderPass::RecordRenderGraph(GDX12RenderGraphBuilder&)
{
    throw std::logic_error(std::string(typeid(*this).name()) + ": RecordRenderGraph is not implemented");
}

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

void GDX12RenderPass::InitializeAll(std::vector<GDX12RenderPass*> passes)
{
    for (auto* pass : passes)
        if (!pass) throw std::invalid_argument("Cannot initialize a null render pass");
    while (!passes.empty())
    {
        const auto remaining = passes.size();
        for (auto it = passes.begin(); it != passes.end();)
        {
            if ((*it)->ValidateInputs())
            {
                (*it)->Initialize();
                it = passes.erase(it);
            }
            else ++it;
        }
        if (passes.size() == remaining)
        {
            std::string message = "Render pass initialization stalled; unresolved inputs:";
            for (auto* pass : passes)
            {
                message += "\n";
                message += typeid(*pass).name();
                if (pass->_inputs.size() < pass->_numInputs) message += " (missing required inputs)";
                for (size_t i = 0; i < pass->_inputs.size(); ++i)
                    if (!pass->_inputs[i] || !pass->_inputs[i]->IsInitialized())
                        message += " [" + std::to_string(i) + "]";
            }
            throw std::runtime_error(message);
        }
    }
}
