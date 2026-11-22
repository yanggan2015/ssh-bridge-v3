// file_browser.h — 左侧“文件”页：串口通道浏览本机共享目录；SSH 通道转 SFTP 面板。
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sb3 {

class AppCore;

class FileBrowser {
public:
    bool create(HINSTANCE inst, HWND parent, AppCore* core, HFONT font, int dpi,
                std::function<void()> open_sftp);
    HWND hwnd() const { return hwnd_; }

    void layout(int x, int y, int w, int h);
    void set_channel(const std::string& name);
    void refresh();

private:
    enum class Kind { None, Serial, Ssh, Unsupported };

    struct Entry {
        std::wstring      name;
        bool              dir  = false;
        unsigned long long size = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void build();
    void layout_children();
    void update_view();
    void load_dir(const std::filesystem::path& dir);
    void fill_list();
    void open_entry(int index);
    void go_up();
    void set_note(const std::wstring& text);
    std::filesystem::path share_root() const;

    AppCore*  core_  = nullptr;
    HINSTANCE inst_  = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      parent_ = nullptr;
    HWND      path_  = nullptr;
    HWND      up_    = nullptr;
    HWND      refresh_ = nullptr;
    HWND      list_  = nullptr;
    HWND      note_  = nullptr;
    HWND      sftp_  = nullptr;
    HFONT     font_  = nullptr;
    int       dpi_   = 96;

    Kind                     kind_ = Kind::None;
    std::string              channel_;
    std::filesystem::path    root_;
    std::filesystem::path    dir_;
    std::vector<Entry>       entries_;
    std::wstring             note_text_;
    std::function<void()>    open_sftp_;
};

}  // namespace sb3
