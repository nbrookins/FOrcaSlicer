#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include "slic3r/GUI/SSWCP.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    // Load the real URL only in load_url(). Creating with Flutter here races the later
    // missing_connection / path=2 LoadURL and cancels the in-flight document (-999).
    m_browser = WebView::CreateWebView(this, "about:blank");
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    // Panel bind so WebViewWebKit's navigation gate (also on the webview) still sees LOADED/ERROR.
    Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this, m_browser->GetId());
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;
    BOOST_LOG_TRIVIAL(warning) << "[FLUTTER_FLOW] [webview] [navigation_requested] view=device url=" << url.ToUTF8().data();

    if (wxGetApp().is_gateway_url(url)) {
        wxGetApp().fltviews().add_printer_view(this, url, apikey);
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
    }

    m_browser->Show();
    WebView::LoadUrl(m_browser, url);

    UpdateState();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

bool PrinterWebView::isSnapmakerPage()
{
    if (m_browser == nullptr)
        return false;
    auto url = m_browser->GetCurrentURL();
    return wxGetApp().is_gateway_url(url);
}

bool PrinterWebView::is_u1_device_page()
{
    if (m_browser == nullptr)
        return false;
    auto url = m_browser->GetCurrentURL();
    return url.find("flutter_web") != std::string::npos &&
           (url.find("path=2") != std::string::npos || url.find("/device_control") != std::string::npos);
}

void PrinterWebView::sendMessage(const std::string& msg) {
    WebView::RunScript(m_browser, msg);
}

void PrinterWebView::update_mode()
{
    // m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    m_browser->EnableAccessToDevTools(true);
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey.IsEmpty())
        return;

    // Re-inject on every document load (e.g. context-menu Reload). Idempotent
    // JS-level marker avoids stacking fetch/XHR wrappers if LOADED fires more than once.
    wxString script = wxString::Format(R"(
    (function() {
        if (window.__sm_apikey_hooked) return;
        window.__sm_apikey_hooked = true;
        var apiKey = '%s';
        // Override fetch to inject X-API-Key header
        if (window.fetch) {
            var originalFetch = window.fetch;
            window.fetch = function(input, init) {
                init = init || {};
                init.headers = init.headers || {};
                if (!init.headers['X-API-Key']) {
                    init.headers['X-API-Key'] = apiKey;
                }
                return originalFetch(input, init);
            };
        }
        // Override XMLHttpRequest to inject X-API-Key header.
        // Preserves prototype chain and static constants for compatibility
        // with libraries that check instanceof or readyState constants.
        var OrigXHR = window.XMLHttpRequest;
        var newXHR = function() {
            var xhr = new OrigXHR();
            var origOpen = xhr.open;
            var headersSet = false;
            xhr.open = function(method, url) {
                origOpen.apply(xhr, arguments);
                if (!headersSet) {
                    xhr.setRequestHeader('X-API-Key', apiKey);
                    headersSet = true;
                }
            };
            return xhr;
        };
        newXHR.prototype = OrigXHR.prototype;
        newXHR.DONE = OrigXHR.DONE;
        newXHR.UNSENT = OrigXHR.UNSENT;
        newXHR.OPENED = OrigXHR.OPENED;
        newXHR.HEADERS_RECEIVED = OrigXHR.HEADERS_RECEIVED;
        newXHR.LOADING = OrigXHR.LOADING;
        window.XMLHttpRequest = newXHR;
    })();
)",
                                       m_apikey);

    // Inject immediately into the current page on all platforms.
    WebView::RunScript(m_browser, script);

#ifndef __WXMAC__
    // On Windows/Linux: also install a persistent user script so the
    // API key is injected at document start on future navigations.
    // AddUserScript works correctly on these platforms (Edge WebView2, WebKitGTK).
    // Do NOT call Reload() — the current page is already handled by RunScript above.
    m_browser->RemoveAllUserScripts();
    m_browser->AddUserScript(script);
#endif
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    evt.Skip();
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
    if (evt.GetURL().IsEmpty())
        return;
    if (evt.GetURL() != m_browser->GetCurrentURL())
        return;
    BOOST_LOG_TRIVIAL(warning) << "[FLUTTER_FLOW] [webview] [navigation_loaded] view=device url=" << evt.GetURL().ToUTF8().data();
    SendAPIKey();
    AutoConnectSavedDevice();
}

// FOS: the Snapmaker device page never reconnects on its own; once per app start, tap the saved
// printer for the user. The page is a Flutter canvas, so the tap goes through Flutter's semantics
// tree (enabled via its placeholder) and is matched by the printer's name. Best effort: if the page
// layout changes and nothing matches, it gives up after ~30 s and the user connects manually.
void PrinterWebView::AutoConnectSavedDevice()
{
    static bool attempted = false;
    if (attempted || m_browser == nullptr || !is_u1_device_page())
        return;
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr || (cfg->has("fos_auto_connect_device") && !cfg->get_bool("fos_auto_connect_device")))
        return;

    json names = json::array();
    try {
        boost::nowide::ifstream in(cfg->config_path());
        json conf = json::parse(in, nullptr, false);
        if (conf.is_object() && conf.contains("devices") && conf["devices"].is_array())
            for (const json &dev : conf["devices"])
                if (dev.is_object() && dev.contains("dev_name") && dev["dev_name"].is_string() && !dev["dev_name"].get<std::string>().empty())
                    names.push_back(dev["dev_name"].get<std::string>());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[FOS] auto-connect: cannot read saved devices: " << e.what();
    }
    if (names.empty())
        return;
    attempted = true;

    const std::string script = R"((function(names) {
    if (window.__fos_autoconnect) return;
    window.__fos_autoconnect = true;
    function report(status) {
        try { window.wx.postMessage(JSON.stringify({ fos_autoconnect: status })); } catch (e) {}
    }
    var tries = 0;
    function tick() {
        tries++;
        var ph = document.querySelector('flt-semantics-placeholder');
        if (ph) ph.click();
        var best = null;
        document.querySelectorAll('[flt-tappable], [role="button"]').forEach(function(el) {
            var label = ((el.getAttribute('aria-label') || '') + ' ' + (el.textContent || '')).trim();
            if (!label) return;
            for (var i = 0; i < names.length; i++) {
                if (label.indexOf(names[i]) >= 0 && (best === null || label.length < best.label.length))
                    best = { el: el, label: label };
            }
        });
        if (best) {
            best.el.click();
            report('clicked: ' + best.label.slice(0, 80));
            return;
        }
        if (tries < 60) setTimeout(tick, 500);
        else report('no matching device element after ' + tries + ' tries');
    }
    setTimeout(tick, 1500);
}))" + std::string("(") + names.dump() + ");";

    BOOST_LOG_TRIVIAL(warning) << "[FOS] auto-connect: tapping saved device " << names.dump();
    WebView::RunScript(m_browser, wxString::FromUTF8(script));
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    // BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    // if (wxGetApp().get_mode() == comDevelop)
    //     wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
