#include "GoogleLoginSheet.hpp"
#include "GoogleAuthManager.hpp"
#include "src/applicationui.hpp"

#include <bb/cascades/Page>
#include <bb/cascades/TitleBar>
#include <bb/cascades/ActionItem>
#include <bb/cascades/WebStorage>
#include <bb/cascades/WebCookieJar>
#include <bb/cascades/WebSettings>
#include <bb/cascades/WebLoadStatus>

GoogleLoginSheet::GoogleLoginSheet() :
        BaseSheet(), webView(0), youtubeTimeout(0), visitingYoutube(false), captured(false)
{
    Page *content = new Page();

    webView = WebView::create();
    webView->setUrl(QUrl("https://accounts.google.com/ServiceLogin?"
            "continue=https://myaccount.google.com"));
    // Cookies are on by default, but be explicit -- the whole point of this
    // sheet is to capture the session cookies afterward.
    webView->settings()->setCookiesEnabled(true);

    QObject::connect(webView, SIGNAL(urlChanged(QUrl)), this, SLOT(onUrlChanged(QUrl)));
    QObject::connect(webView, SIGNAL(loadingChanged(bb::cascades::WebLoadRequest*)), this,
            SLOT(onLoadingChanged(bb::cascades::WebLoadRequest*)));

    // Safety net: if the youtube.com visit never reports "succeeded" (slow
    // network, consent page, redirect loop), capture whatever we have
    // instead of leaving the user stuck on a blank sheet.
    youtubeTimeout = new QTimer(this);
    youtubeTimeout->setSingleShot(true);
    youtubeTimeout->setInterval(20000);
    QObject::connect(youtubeTimeout, SIGNAL(timeout()), this, SLOT(onYoutubeVisitTimeout()));

    TitleBar *titleBar = new TitleBar(TitleBarKind::Default);
    ActionItem *cancelAction = ActionItem::create().title("Cancel");
    QObject::connect(cancelAction, SIGNAL(triggered()), this, SLOT(closeActionClick()));
    titleBar->setTitle("Sign in to Google");
    titleBar->setDismissAction(cancelAction);

    content->setTitleBar(titleBar);
    content->setContent(webView);
    this->setContent(content);

    open();
}

void GoogleLoginSheet::onUrlChanged(const QUrl &url)
{
    if (captured || visitingYoutube) {
        return;
    }

    // myaccount.google.com is only reached after a *completed* sign-in --
    // intermediate steps (password entry, 2FA challenge, "verify it's you"
    // prompts) all stay on accounts.google.com. This is a heuristic, not
    // an official signal; see header comment.
    if (url.host() == "myaccount.google.com") {
        startYoutubeVisit();
    }
}

void GoogleLoginSheet::startYoutubeVisit()
{
    // Signed in to Google, but YouTube has not yet issued its own
    // youtube.com cookies in this WebView. Load it so it does.
    visitingYoutube = true;
    youtubeTimeout->start();
    webView->setUrl(QUrl("https://www.youtube.com/"));
}

void GoogleLoginSheet::onLoadingChanged(bb::cascades::WebLoadRequest *loadRequest)
{
    if (captured || !visitingYoutube || !loadRequest) {
        return;
    }

    if (loadRequest->status() == bb::cascades::WebLoadStatus::Succeeded
            && webView->url().host().endsWith("youtube.com")) {
        captureSessionAndClose();
    }
}

void GoogleLoginSheet::onYoutubeVisitTimeout()
{
    if (!captured && visitingYoutube) {
        captureSessionAndClose();
    }
}

// Adds every "Name=Value" entry of `source` to `pairs`, replacing an earlier
// entry with the same name (so later sources win, e.g. youtube.com over
// google.com for cookies both domains set).
void GoogleLoginSheet::mergeCookies(const QStringList &source, QStringList &names,
        QStringList &pairs)
{
    for (int i = 0; i < source.count(); i++) {
        int eq = source[i].indexOf('=');
        if (eq <= 0) {
            continue;
        }
        QString name = source[i].left(eq).trimmed();
        int existing = names.indexOf(name);
        if (existing >= 0) {
            pairs[existing] = source[i];
        } else {
            names.append(name);
            pairs.append(source[i]);
        }
    }
}

void GoogleLoginSheet::captureSessionAndClose()
{
    if (captured) {
        return;
    }
    captured = true;
    youtubeTimeout->stop();

    bb::cascades::WebCookieJar *jar = webView->storage()->cookieJar();

    QStringList names;
    QStringList cookies;
    mergeCookies(jar->cookiesForUrl(QUrl("https://www.google.com")), names, cookies);
    mergeCookies(jar->cookiesForUrl(QUrl("https://www.youtube.com")), names, cookies);

    // Email label is left blank for now -- Settings will show a generic
    // "Logged in" state rather than "Logged in as name@gmail.com". Scraping
    // the email out of the post-login page via evaluateJavaScript() is
    // possible as a follow-up (requires wiring up the
    // javaScriptResult(int,QVariant) signal to correlate the async result
    // with this call, which didn't fit in this pass) but isn't needed for
    // login/cookie-capture to work.
    if (cookies.isEmpty()) {
        // Landed on myaccount.google.com but the cookie jar came back
        // empty -- something unexpected happened (e.g. domain mismatch).
        // Don't save a broken session; let the user retry.
        closeSheet();
        return;
    }

    ApplicationUI::googleAuthManager->saveSession(cookies, "");

    closeSheet();
}
