// Garage wheels in the parking area (--parking_area_garage_wheels;
// src/hooks/parking_area_wheels.cpp).

#pragma once

namespace hooks {

// Main thread. Whether the car scene set up last is the parking area's and
// its wheels are to be the garage's (set a's) instead of the race's (set
// b's). Taken when the scene is set up, so a change to the flag applies from
// the next scene.
bool ParkingArea_GarageWheels();

}  // namespace hooks
