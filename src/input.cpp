// input.cpp
#include "input.h"

void Input::poll(){
    // Keyboard
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    uint8_t bits=0;
    bits |= k[SDL_SCANCODE_Z]        ? (1<<0) : 0; // A
    bits |= k[SDL_SCANCODE_X]        ? (1<<1) : 0; // B
    bits |= k[SDL_SCANCODE_RSHIFT]   ? (1<<2) : 0; // Select
    bits |= k[SDL_SCANCODE_RETURN]   ? (1<<3) : 0; // Start
    bits |= k[SDL_SCANCODE_UP]       ? (1<<4) : 0; // Up
    bits |= k[SDL_SCANCODE_DOWN]     ? (1<<5) : 0; // Down
    bits |= k[SDL_SCANCODE_LEFT]     ? (1<<6) : 0; // Left
    bits |= k[SDL_SCANCODE_RIGHT]    ? (1<<7) : 0; // Right

    // Optional game controller (hotplug from your main loop)
    if(controller){
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A)      ? (1<<0) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B)      ? (1<<1) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)   ? (1<<2) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START)  ? (1<<3) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP)    ? (1<<4) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  ? (1<<5) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  ? (1<<6) : 0;
        bits |= SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? (1<<7) : 0;
    }

    padState = bits;
    if(strobe) shift1 = padState;
}
