#include "EditorHost.h"
namespace nytera
{
Status EditorHost::open(HWND parent)
{
    close();
    if (!IsWindow(parent)) return Failure{ErrorCode::invalidData, "Invalid host editor parent"};
    previousContext = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent));
    vst_rect_t* rectangle = nullptr;
    source.dispatch(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rectangle);
    if (rectangle && rectangle->right > rectangle->left && rectangle->bottom > rectangle->top)
        dimensions = {static_cast<uint32_t>(rectangle->right - rectangle->left), static_cast<uint32_t>(rectangle->bottom - rectangle->top)};
    container = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, static_cast<int>(dimensions.width), static_cast<int>(dimensions.height), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!container) { close(); return Failure{ErrorCode::plugin, "Cannot create native editor container"}; }
    try
    {
        opened = true;
        auto initialized = source.openEditor(container);
        if (auto* failure = std::get_if<Failure>(&initialized)) { close(); return *failure; }
        source.dispatch(VST_EFFECT_OPCODE_EDITOR_GET_RECT, 0, 0, &rectangle);
        if (rectangle && rectangle->right > rectangle->left && rectangle->bottom > rectangle->top)
            resize({static_cast<uint32_t>(rectangle->right - rectangle->left), static_cast<uint32_t>(rectangle->bottom - rectangle->top)});
        return std::monostate{};
    }
    catch (const std::exception& error) { close(); return Failure{ErrorCode::plugin, error.what()}; }
}
void EditorHost::close() noexcept
{
    if (opened)
    {
        try { source.dispatch(VST_EFFECT_OPCODE_EDITOR_CLOSE); } catch (...) {}
        opened = false;
    }
    if (container) { DestroyWindow(container); container = nullptr; }
    if (previousContext) { SetThreadDpiAwarenessContext(previousContext); previousContext = nullptr; }
}
void EditorHost::idle()
{
    if (opened) source.dispatch(VST_EFFECT_OPCODE_EDITOR_KEEP_ALIVE);
}
void EditorHost::resize(Size next)
{
    if (next.width == 0 || next.height == 0 || next.width > 32768 || next.height > 32768) return;
    dimensions = next;
    if (container) SetWindowPos(container, nullptr, 0, 0, static_cast<int>(next.width), static_cast<int>(next.height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
}
