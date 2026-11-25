#pragma once

#include "Classic/VF50ClassicController.h"

namespace vf50::state {

using VF50ClassicStateMachine = vf50::classic::VF50ClassicController;
using ClassicLedState        = vf50::classic::LedState;
using ClassicLedSnapshot     = vf50::classic::ClassicLedSnapshot;
using ClassicScreen          = vf50::classic::ClassicScreen;
using ClassicLcdFrame        = vf50::classic::ClassicLcdFrame;

} // namespace vf50::state
