#pragma once

#include <QString>

namespace Janna {

struct LcuCredentials {
    QString port;
    QString token;

    [[nodiscard]] bool isValid() const { return !port.isEmpty() && !token.isEmpty(); }
};

class LcuCredentialProvider {
public:
    // Credentials are discovered for each request and are never persisted.
    static LcuCredentials discover(QString &error);
};

} // namespace Janna
