#pragma once

#include "Settings.h"

namespace RE {
    class InputEvent;
}

namespace DragLook {
    bool Install(const Settings& a_settings);

    void Arm(bool a_armed);

    // Ends a drag whose release another menu took
    void Cancel();

    // True while dragging
    bool Update(RE::InputEvent* a_head);
}
