#include "SDLInputManager.hpp"
#include "SDL.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_joystick.h"

#include <algorithm>
#include <optional>

SDLInputManager::SDLInputManager()
{
    qRegisterMetaType<SDL_Event>();
    work_event_id = SDL_RegisterEvents(1);
    rumble_event_id = SDL_RegisterEvents(1);
}

SDLInputManager::~SDLInputManager()
{
}

void SDLInputManager::AddDevice(int device_index)
{
    std::unique_lock lk{mtx};
    SDLInputDevice d;
    if (!d.open(device_index))
        return;
    d.index = FindFirstOpenIndex();

    printf("Slot %d: %s: ", d.index, SDL_JoystickName(d.joystick));
    printf("%zu axes, %zu buttons, %zu hats, %s API\n", d.axes.size(), d.buttons.size(), d.hats.size(), d.is_controller ? "Controller" : "Joystick");

    devices.insert({ d.instance_id, d });
}

void SDLInputManager::RemoveDevice(int instance_id)
{
    std::unique_lock lk{mtx};
    auto iter = devices.find(instance_id);
    if (iter == devices.end())
        return;

    auto &d = iter->second;

    if (d.is_controller)
        SDL_GameControllerClose(d.controller);
    else
        SDL_JoystickClose(d.joystick);

    devices.erase(iter);
    return;
}

std::optional<SDLInputManager::DiscreteHatEvent>
SDLInputManager::DiscretizeHatEvent(SDL_Event &event)
{
    std::unique_lock lk{mtx};
    auto &device = devices.at(event.jhat.which);
    auto &hat = event.jhat.hat;
    auto new_state = event.jhat.value;
    auto &old_state = device.hats[hat].state;

    if (old_state == new_state)
        return std::nullopt;

    DiscreteHatEvent dhe{};
    dhe.hat = hat;
    dhe.joystick_num = device.index;

    for (auto &s : { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT })
        if ((old_state & s) != (new_state & s))
        {
            printf(" old: %d, new: %d\n", old_state, new_state);
            dhe.direction = s;
            dhe.pressed = (new_state & s);
            old_state = new_state;
            return dhe;
        }

    return std::nullopt;
}

std::optional<SDLInputManager::DiscreteAxisEvent>
SDLInputManager::DiscretizeJoyAxisEvent(SDL_Event &event)
{
    std::unique_lock {mtx};
    auto &device = devices.at(event.jaxis.which);
    auto &axis = event.jaxis.axis;
    auto now = event.jaxis.value;
    auto &then = device.axes[axis].last;
    auto center = device.axes[axis].initial;

    int offset = now - center;

    auto pressed = [&](int axis) -> int {
        if (axis > (center + (32767 - center) / 3)) // TODO threshold
            return 1;
        if (axis < (center - (center + 32768) / 3)) // TODO threshold
            return -1;
        return 0;
    };

    auto was_pressed_then = pressed(then);
    auto is_pressed_now   = pressed(now);

    if (was_pressed_then == is_pressed_now)
    {
        then = now;
        return std::nullopt;
    }

    DiscreteAxisEvent dae;
    dae.axis = axis;
    dae.direction = is_pressed_now ? is_pressed_now : was_pressed_then;
    dae.pressed = (is_pressed_now != 0);
    dae.joystick_num = device.index;
    then = now;

    return dae;
}

void SDLInputManager::run()
{
    bool run = true;
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    SDL_Event e{};
    std::optional<int> rumble_end;

    while (run)
    {
        if (rumble_end)
        {
            auto timeout = !SDL_WaitEventTimeout(
                    &e, std::max<int>(*rumble_end - SDL_GetTicks(), 0));
            if (timeout)
            {
                rumble_end = std::nullopt;
                for (const auto [id, dev] : devices)
                    SDL_GameControllerRumble(dev.controller, 0, 0, 0);
            }
        }
        else
            SDL_WaitEvent(&e);
        switch (e.type)
        {
        case SDL_JOYAXISMOTION:
        case SDL_JOYHATMOTION:
            emit event(e, 0);
            break;
        case SDL_JOYBUTTONUP:
        case SDL_JOYBUTTONDOWN:
            {
                std::unique_lock lk{mtx};
                emit event(e, devices[e.jbutton.which].index);
            }
            break;
        case SDL_JOYDEVICEADDED:
            AddDevice(e.jdevice.which);
            emit event(e, 0);
            break;
        case SDL_JOYDEVICEREMOVED:
            RemoveDevice(e.jdevice.which);
            emit event(e, 0);
            break;
        case SDL_QUIT:
            run = false;
            break;
        default:
            if (e.type == work_event_id)
            {
                std::unique_lock lk{mtx};
                if (on_thread)
                    on_thread(devices);
                cv.notify_one();
            } else if (e.type == rumble_event_id)
            {
                std::unique_lock lk{mtx};
                const auto [low_freq, high_freq, duration_ms] = rumble_data;
                rumble_end = SDL_GetTicks() + duration_ms;
                for (const auto [id, dev] : devices)
                    SDL_GameControllerRumble(dev.controller, low_freq,
                                             high_freq, duration_ms);
            }
            break;
        }
    }
    SDL_Quit();
}

int SDLInputManager::FindFirstOpenIndex()
{
    for (int i = 0;; i++)
    {
        if (std::none_of(devices.begin(), devices.end(), [i](auto &d) -> bool {
            return (d.second.index == i);
        }))
            return i;
    }
    return -1;
}

bool SDLInputDevice::open(int joystick_num)
{
    sdl_joystick_number = joystick_num;
    is_controller = SDL_IsGameController(joystick_num);

    if (is_controller)
    {
        controller = SDL_GameControllerOpen(joystick_num);
        joystick = SDL_GameControllerGetJoystick(controller);
    }
    else
    {
        joystick = SDL_JoystickOpen(joystick_num);
        controller = nullptr;
    }

    if (!joystick)
        return false;

    auto num_axes = SDL_JoystickNumAxes(joystick);
    axes.resize(num_axes);
    for (int i = 0; i < num_axes; i++)
    {
        SDL_JoystickGetAxisInitialState(joystick, i, &axes[i].initial);
        axes[i].last = axes[i].initial;
    }

    buttons.resize(SDL_JoystickNumButtons(joystick));
    hats.resize(SDL_JoystickNumHats(joystick));
    instance_id = SDL_JoystickInstanceID(joystick);

    return true;
}

std::vector<std::pair<int, std::string>> SDLInputManager::getXInputControllers()
{
    std::unique_lock lk{mtx};
    std::vector<std::pair<int, std::string>> list;

    for (auto &d : devices)
    {
        if (!d.second.is_controller)
            continue;

        list.push_back(std::pair<int, std::string>(d.first, SDL_JoystickName(d.second.joystick)));
        auto bind = SDL_GameControllerGetBindForButton(d.second.controller, SDL_CONTROLLER_BUTTON_A);
    }

    return list;
}

void SDLInputManager::runInSDLThread(on_thread_t func)
{
    std::unique_lock lk{mtx};
    this->on_thread = func;
    SDL_Event e{.type=work_event_id};
    SDL_PushEvent(&e);
    cv.wait(lk);
}

void SDLInputManager::stop()
{
    SDL_Event e{.type=SDL_QUIT};
    SDL_PushEvent(&e);
    wait();
}

void SDLInputManager::rumble(uint16_t low_freq, uint16_t high_freq, uint32_t duration_ms)
{
    std::unique_lock lk{mtx};
    rumble_data = std::make_tuple(low_freq, high_freq, duration_ms);
    SDL_Event e{.type=rumble_event_id};
    SDL_PushEvent(&e);
}
