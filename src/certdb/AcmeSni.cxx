/*
 * Copyright 2007-2018 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "AcmeSni.hxx"
#include "ssl/Dummy.hxx"
#include "ssl/Error.hxx"
#include "ssl/Edit.hxx"

static std::string
MakeHandleFromAcmeSni01(const std::string &acme)
{
    auto i = acme.find('.');
    if (i != 32)
        i = std::min<size_t>(32, acme.length());

    return "acme.invalid:" + acme.substr(0, i);
}

UniqueX509
MakeTlsSni01Cert(EVP_PKEY &account_key, EVP_PKEY &key,
                 const AcmeClient::AuthzChallenge &authz)
{
    const auto alt_host = authz.MakeDnsName(account_key);
    std::string alt_name = "DNS:" + alt_host;

    const std::string common_name = MakeHandleFromAcmeSni01(alt_host);

    auto cert = MakeSelfIssuedDummyCert(common_name.c_str());

    AddExt(*cert, NID_subject_alt_name, alt_name.c_str());

    X509_set_pubkey(cert.get(), &key);
    if (!X509_sign(cert.get(), &key, EVP_sha1()))
        throw SslError("X509_sign() failed");

    return cert;
}

