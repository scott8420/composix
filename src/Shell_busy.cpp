#include "Shell.hpp"
#include "Log.hpp"

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

// Shell_busy.cpp -- HELPERS for s102: a progress dialog in front of work
// long enough to look like a hang (Scott: "show a progress dialog so the
// user doesn't think there is an issue").
//
// Two kinds of work:
//   * busy(): on the UI thread (it touches the document and the views) --
//     the dialog is put up and drawn FIRST, then the work runs; it can't
//     animate meanwhile, but it says what's happening.
//   * busy_thread(): a job on a worker thread that reports a fraction and
//     a line; the dialog's bar follows it; `done` then runs on the UI
//     thread. The job must not touch the Shell's document or views.

namespace composix {

namespace {
struct BusyUi {
    Gtk::Window*      win = nullptr;
    Gtk::Label*       what = nullptr;
    Gtk::ProgressBar* bar = nullptr;
};

BusyUi make_busy(Gtk::Window& parent, const std::string& title, const std::string& what) {
    BusyUi u;
    u.win = new Gtk::Window();
    u.win->set_name("busy");
    u.win->set_transient_for(parent);
    u.win->set_modal(true);
    u.win->set_deletable(false);
    u.win->set_resizable(false);
    u.win->set_title(title);
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(22);
    box->set_size_request(360, -1);
    auto head = Gtk::make_managed<Gtk::Label>(title);
    head->add_css_class("title-4");
    head->set_xalign(0);
    u.what = Gtk::make_managed<Gtk::Label>(what);
    u.what->set_xalign(0);
    u.what->add_css_class("dim-label");
    u.bar = Gtk::make_managed<Gtk::ProgressBar>();
    u.bar->set_pulse_step(0.08);
    box->append(*head);
    box->append(*u.what);
    box->append(*u.bar);
    u.win->set_child(*box);
    // Closing (Esc / the compositor) does nothing: the work can't be stopped.
    u.win->signal_close_request().connect([] { return true; }, false);
    return u;
}

void close_busy(BusyUi& u) {
    if (!u.win) return;
    u.win->set_visible(false);
    delete u.win;
    u.win = nullptr;
}

// Run `f` once the dialog has been drawn: a frame after it maps.
void after_drawn(Gtk::Window* win, std::function<void()> f) {
    win->signal_map().connect([f] {
        Glib::signal_timeout().connect_once(f, 120);
    }, true);
    win->present();
}
}  // namespace

void Shell::busy(const std::string& title, const std::string& what, std::function<void()> work) {  // helper
    auto u = std::make_shared<BusyUi>(make_busy(*this, title, what));
    u->bar->pulse();
    after_drawn(u->win, [u, work] {
        work();
        close_busy(*u);
    });
}

void Shell::busy_thread(const std::string& title, const std::string& what, std::function<void(const Report&)> job,
                        const std::string& then_what, std::function<void()> done) {  // helper
    struct Shared {
        std::mutex        mx;
        double            fraction = 0;
        std::string       line;
        std::atomic<bool> finished = false;
        std::thread       worker;
    };
    auto u = std::make_shared<BusyUi>(make_busy(*this, title, what));
    auto sh = std::make_shared<Shared>();
    after_drawn(u->win, [u, sh, job, then_what, done] {
        sh->worker = std::thread([sh, job] {
            job([sh](double f, const std::string& line) {
                std::lock_guard lk(sh->mx);
                sh->fraction = f;
                if (!line.empty()) sh->line = line;
            });
            sh->finished = true;
        });
        Glib::signal_timeout().connect([u, sh, then_what, done] {
            {
                std::lock_guard lk(sh->mx);
                if (sh->fraction > 0) u->bar->set_fraction(sh->fraction); else u->bar->pulse();
                if (!sh->line.empty()) u->what->set_text(sh->line);
            }
            if (!sh->finished) return true;
            sh->worker.join();
            // The UI-thread part: say so, let it draw, then do it.
            u->bar->set_fraction(1);
            if (!then_what.empty()) u->what->set_text(then_what);
            Glib::signal_timeout().connect_once([u, done] {
                done();
                close_busy(*u);
            }, 120);
            return false;
        }, 80);
    });
}

}  // namespace composix
