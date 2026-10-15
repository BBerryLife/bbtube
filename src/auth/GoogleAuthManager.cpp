#include "GoogleAuthManager.hpp"
#include "CookieCrypto.hpp"
#include "src/db/DbHelper.hpp"

GoogleAuthManager::GoogleAuthManager(QObject *parent) :
        QObject(parent), loggedIn(false), cacheValid(false)
{
    loadFromDb();
}

void GoogleAuthManager::loadFromDb()
{
    QString email = DbHelper::getGoogleAuthValue("email");
    QString cipherText = DbHelper::getGoogleAuthValue("cookie_enc");
    QString iv = DbHelper::getGoogleAuthValue("cookie_iv");

    // Presence of a stored cipher text is what defines "logged in" --
    // deliberately not decrypting here at startup (decryption happens
    // lazily in cookieHeader()/sapisid(), the only places that actually
    // need the plaintext), so a corrupt/foreign-device blob still shows
    // the user as logged in (matching what's in the DB) rather than
    // silently logging them out; the cookie-attachment code path is what
    // falls back to anonymous requests if decryption then fails.
    loggedIn = !cipherText.isEmpty() && !iv.isEmpty();
    loggedInEmail = email;
}

void GoogleAuthManager::saveSession(const QStringList &rawCookies, const QString &email)
{
    QString cookieHeaderStr = rawCookies.join("; ");

    QString cipherText;
    QString iv;
    if (!CookieCrypto::encrypt(cookieHeaderStr, &cipherText, &iv)) {
        // Platform RNG failure -- extremely unlikely. Don't half-save.
        return;
    }

    DbHelper::setGoogleAuthValue("cookie_enc", cipherText);
    DbHelper::setGoogleAuthValue("cookie_iv", iv);
    DbHelper::setGoogleAuthValue("email", email);

    loggedIn = true;
    loggedInEmail = email;
    cachedCookieHeader = cookieHeaderStr;
    cacheValid = true;

    emit loginStateChanged(true, email);
}

void GoogleAuthManager::logout()
{
    DbHelper::clearGoogleAuth();

    loggedIn = false;
    loggedInEmail = "";
    cachedCookieHeader = "";
    cacheValid = false;

    emit loginStateChanged(false, "");
}

QString GoogleAuthManager::cookieHeader()
{
    if (!loggedIn) {
        return "";
    }
    if (cacheValid) {
        return cachedCookieHeader;
    }

    QString cipherText = DbHelper::getGoogleAuthValue("cookie_enc");
    QString iv = DbHelper::getGoogleAuthValue("cookie_iv");

    QString plaintext;
    if (!CookieCrypto::decrypt(cipherText, iv, &plaintext)) {
        // Wrong device key, corrupt blob, etc. -- don't crash or retry in
        // a loop; caller treats "" as "send request without cookies".
        return "";
    }

    cachedCookieHeader = plaintext;
    cacheValid = true;
    return cachedCookieHeader;
}

QString GoogleAuthManager::extractCookieValue(const QString &cookieHeaderStr, const QString &name)
{
    QStringList parts = cookieHeaderStr.split("; ");
    QString prefix = name + "=";
    for (int i = 0; i < parts.count(); i++) {
        if (parts[i].startsWith(prefix)) {
            return parts[i].mid(prefix.length());
        }
    }
    return "";
}

QString GoogleAuthManager::sapisid()
{
    QString header = cookieHeader();
    if (header.isEmpty()) {
        return "";
    }
    // Prefer the __Secure-3PAPISID variant if present (this is what
    // current YouTube web itself signs requests with when both are set),
    // falling back to plain SAPISID.
    QString value = extractCookieValue(header, "__Secure-3PAPISID");
    if (!value.isEmpty()) {
        return value;
    }
    return extractCookieValue(header, "SAPISID");
}

QString GoogleAuthManager::cookieValue(const QString &name)
{
    QString header = cookieHeader();
    if (header.isEmpty()) {
        return "";
    }
    return extractCookieValue(header, name);
}

bool GoogleAuthManager::hasYoutubeSession()
{
    return !cookieValue("LOGIN_INFO").isEmpty();
}
