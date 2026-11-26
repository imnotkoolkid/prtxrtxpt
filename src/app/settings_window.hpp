#pragma once
#include "renderer/renderer.hpp"
class SettingsWindow
{
  public:
    HWND window{};
    void create(HWND owner, Renderer &);
    void toggle();

  private:
    Renderer *renderer{};
    HWND group{}, preset{}, list{}, slider{}, number{}, action{}, label{};
    bool refreshing{};
    Parameter selected = Enabled;
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    void populate();
    void refresh();
    void layout();
    void editValue(int);
};
