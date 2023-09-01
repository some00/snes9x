#pragma once

#include "SDL.h"
#include <QMetaType>
#include <QThread>
#include <map>
#include <vector>
#include <string>
#include <optional>
#include <thread>
#include <condition_variable>

struct SDLInputDevice
{
    bool open(int joystick_num);

    int index;
    int sdl_joystick_number;
    bool is_controller;
    SDL_GameController *controller = nullptr;
    SDL_Joystick *joystick = nullptr;
    SDL_JoystickID instance_id;

    struct Axis
    {
        int16_t initial;
        int last;
    };
    std::vector<Axis> axes;

    struct Hat
    {
        uint8_t state;
    };
    std::vector<Hat> hats;

    std::vector<bool> buttons;
};

struct SDLInputManager : QThread
{
    Q_OBJECT
public:
    using devices_t = std::map<SDL_JoystickID, SDLInputDevice>;
    using on_thread_t = std::function<void(devices_t&)>;

    SDLInputManager();
    ~SDLInputManager();

    std::optional<SDL_Event> ProcessEvent() { return std::nullopt; }
    std::vector<std::pair<int, std::string>> getXInputControllers();

    struct DiscreteAxisEvent
    {
        int joystick_num;
        int axis;
        int direction;
        int pressed;
    };
    std::optional<DiscreteAxisEvent> DiscretizeJoyAxisEvent(SDL_Event &event);

    struct DiscreteHatEvent
    {
        int joystick_num;
        int hat;
        int direction;
        bool pressed;
    };
    std::optional<DiscreteHatEvent> DiscretizeHatEvent(SDL_Event &event);

    void runInSDLThread(on_thread_t func);
    void stop();
signals:
    void event(SDL_Event, int);
protected:
    void run() override;
private:
    void AddDevice(int i);
    void RemoveDevice(int i);
    int FindFirstOpenIndex();

    std::map<SDL_JoystickID, SDLInputDevice> devices;
    std::mutex mtx;
    std::condition_variable cv;
    uint32_t work_event_id;
    on_thread_t on_thread;
};
