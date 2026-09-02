#include <gtest/gtest.h>

#include "trtc_asr/credential.h"
#include "trtc_asr/errors.h"

TEST(Site, HostForSiteMapsKnownValues) {
  EXPECT_EQ(trtc_asr::HostForSite(""), trtc_asr::kHostCN);
  EXPECT_EQ(trtc_asr::HostForSite(trtc_asr::kSiteCN), trtc_asr::kHostCN);
  EXPECT_EQ(trtc_asr::HostForSite("CN"), trtc_asr::kHostCN);
  EXPECT_EQ(trtc_asr::HostForSite(" cn "), trtc_asr::kHostCN);
  EXPECT_EQ(trtc_asr::HostForSite(trtc_asr::kSiteIntl), trtc_asr::kHostIntl);
  EXPECT_EQ(trtc_asr::HostForSite("INTL"), trtc_asr::kHostIntl);
  try {
    trtc_asr::HostForSite("mars");
    FAIL() << "expected invalid param";
  } catch (const trtc_asr::ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrInvalidParam);
  }
}

TEST(Site, ResolveHelpersHonorOverrideAndSite) {
  EXPECT_EQ(trtc_asr::ResolveWSEndpoint("", trtc_asr::kSiteIntl),
            std::string("wss://") + trtc_asr::kHostIntl);
  EXPECT_EQ(trtc_asr::ResolveHTTPEndpoint("", ""),
            std::string("https://") + trtc_asr::kHostCN);
  EXPECT_EQ(trtc_asr::ResolveWSEndpoint("wss://mock.local", trtc_asr::kSiteIntl),
            "wss://mock.local");
}

TEST(Site, CredentialSetSite) {
  trtc_asr::Credential c(1, 2, "k");
  EXPECT_TRUE(c.site().empty());
  c.set_site(trtc_asr::kSiteIntl);
  EXPECT_EQ(c.site(), trtc_asr::kSiteIntl);
}
