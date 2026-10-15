#ifndef GOOGLELOGINSHEET_HPP_
#define GOOGLELOGINSHEET_HPP_

#include "src/utils/BaseSheet.hpp"

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <bb/cascades/WebView>
#include <bb/cascades/WebLoadRequest>

using namespace bb::cascades;

// Shows accounts.google.com in an embedded WebView so the user can sign in
// with their real Google account. Once the WebView navigates to
// myaccount.google.com (the page Google redirects to after a *completed*
// sign-in -- as opposed to intermediate steps like the password or 2FA
// pages, which stay on accounts.google.com), this sheet reads the session
// cookies out of the WebView's cookie jar, hands them to
// ApplicationUI::googleAuthManager to encrypt + persist, and closes itself.
//
// After sign-in completes the sheet also visits www.youtube.com in the same
// WebView before capturing, so YouTube sets its own youtube.com-domain
// cookies (LOGIN_INFO etc). Those are what YouTube's InnerTube API actually
// checks to treat a request as signed in -- the google.com cookies alone are
// not enough. Cookies from both domains are merged into the saved session.
//
// This is a best-effort heuristic, not an official API -- Google doesn't
// expose a "login succeeded" callback to embedded WebViews. If Google ever
// changes the post-login redirect target, this will need updating (see
// onUrlChanged()).
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class GoogleLoginSheet: public BaseSheet
{
    Q_OBJECT
public:
    GoogleLoginSheet();
    virtual ~GoogleLoginSheet()
    {
    }

private slots:
    void onUrlChanged(const QUrl &url);
    void onLoadingChanged(bb::cascades::WebLoadRequest *loadRequest);
    void onYoutubeVisitTimeout();

private:
    void startYoutubeVisit();
    void captureSessionAndClose();
    static void mergeCookies(const QStringList &source, QStringList &names, QStringList &pairs);

    WebView *webView;
    QTimer *youtubeTimeout;
    bool visitingYoutube;
    bool captured; // guards against firing twice on rapid subsequent navigations
};

#endif /* GOOGLELOGINSHEET_HPP_ */
