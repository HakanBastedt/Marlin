#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "boards.h"
#include "macros.h"

#define HAKANS_CO2

#ifdef HAKANS_CO2
#include "Conf_lasCO2.h"
#else
#include "Conf_lasdiod.h"
#endif

#include "Configuration_adv.h"
#include "thermistortables.h"

#endif //CONFIGURATION_H
