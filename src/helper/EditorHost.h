#pragma once
#include "Vst2Instance.h"
namespace nytera
{
class EditorHost
{
public:
    explicit EditorHost(Vst2Instance& source) : source(source) {}
    ~EditorHost() { close(); }
    Status open(HWND parent);
    void close() noexcept;
    void idle();
    void resize(Size);
    Size size() const { return dimensions; }
    HWND window() const { return container; }
private:
    Vst2Instance& source;
    HWND container = nullptr;
    Size dimensions;
    bool opened = false;
    DPI_AWARENESS_CONTEXT previousContext = nullptr;
};
}
