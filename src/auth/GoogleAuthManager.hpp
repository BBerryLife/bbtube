#ifndef GOOGLEAUTHMANAGER_HPP_
#define GOOGLEAUTHMANAGER_HPP_

#include <QObject>
#include <QString>
#include <QStringList>

// Owns the app's single Google login session: persisting the encrypted
// cookie string (via DbHelper + CookieCrypto), exposing it decrypted for
// the /player request-signing code, and notifying the UI (SettingsSheet)
// when login state changes.
//
// This does NOT do any network requests or SAPISIDHASH signing itself --
// see GoogleLoginSheet for the WebView-based login flow that feeds this,
// and YoutubeClient for where the cookie header actually gets attached to
// /player requests (separate follow-up).
//
// One instance is owned by ApplicationUI (ApplicationUI::googleAuthManager,
// analogous to ApplicationUI::appSettings) and lives for the app's lifetime.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class GoogleAuthManager: public QObject
{
Q_OBJECT
public:
    explicit GoogleAuthManager(QObject *parent = 0);
    virtual ~GoogleAuthManager()
    {
    }

    bool isLoggedIn() const
    {
        return loggedIn;
    }

    // Empty string if not logged in.
    QString email() const
    {
        return loggedInEmail;
    }

    // Called by GoogleLoginSheet once it has captured cookies from the
    // WebView's cookie jar after a successful sign-in. `rawCookies` is the
    // list of "Name=Value" strings as returned by
    // WebCookieJar::cookiesForUrl() for the .google.com domain.
    // `email` is best-effort (scraped from the post-login page); pass an
    // empty string if it couldn't be determined -- login still succeeds,
    // just without a friendly label.
    void saveSession(const QStringList &rawCookies, const QString &email);

    // Clears the stored session (both in-memory and in the DB). Called
    // from the Settings sheet's "Log out" action.
    void logout();

    // Returns the cookie header value to attach to InnerTube requests
    // (e.g. "SID=...; HSID=...; SSID=...; APISID=...; SAPISID=..."), or
    // an empty string if not logged in / decryption failed. Decryption
    // failure (e.g. DB copied from another device -- see CookieCrypto) is
    // treated the same as "not logged in": this returns "" and the caller
    // falls back to the existing anonymous request path.
    QString cookieHeader();

    // The raw SAPISID cookie value alone, needed to compute the
    // Authorization: SAPISIDHASH header YouTube requires on authenticated
    // InnerTube requests. Returns "" if not logged in or not present.
    QString sapisid();

    // Returns the value of a single cookie from the stored session by name
    // (e.g. "__Secure-1PAPISID"), or "" if not logged in / not present.
    QString cookieValue(const QString &name);

    // True if the stored session contains the cookies YouTube itself needs
    // to treat a request as signed in (LOGIN_INFO only exists on the
    // youtube.com domain, so this is false for sessions captured before
    // GoogleLoginSheet started visiting youtube.com).
    bool hasYoutubeSession();

signals:
    void loginStateChanged(bool loggedIn, QString email);

private:
    void loadFromDb();
    static QString extractCookieValue(const QString &cookieHeaderStr, const QString &name);

    bool loggedIn;
    QString loggedInEmail;
    // Decrypted cookie header, cached in memory after first successful
    // decrypt this run so we don't re-derive the AES key / hit the DB on
    // every single /player request.
    QString cachedCookieHeader;
    bool cacheValid;
};

#endif /* GOOGLEAUTHMANAGER_HPP_ */
