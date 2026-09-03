#pragma once

#include "FlyerSource.h"

// The one adapter shipped today: the digital-flyer aggregator that every major
// Quebec banner publishes through (IGA, Metro, Super C, Maxi, Provigo, Adonis,
// Jean Coutu, Pharmaprix and roughly ninety more per postal code).
//
// The API is undocumented and unversioned. Every assumption it makes about the
// payload is defensive: a missing or retyped field degrades one item, never the
// sync. See FlyerSource.h for why it is isolated behind an interface.
namespace source
{

// Reduces a product link to the merchant's own address.
//
// Two of the four Quebec grocery banners publish theirs wrapped in a DoubleClick
// click-through — Super C wraps it twice — so opening the string as given would
// register a click with an advertising network on behalf of a user who only
// asked to see a product page.
//
// Returns an EMPTY string whenever it cannot reach a plain destination it
// trusts. That direction matters: an earlier draft returned the original on
// failure, which meant the one function written to avoid the tracker would open
// it in exactly the cases it did not understand.
//
// Exposed rather than file-local so the regression tests can reach it.
std::string stripTracking(const std::string& url);

// Moves a per-weight rate out of the description and into the price text.
//
// Walmart states the unit nowhere near the price fields: price_text, pre and
// post all arrive empty, and the description reads "Poulet canadien à 100 %.
// N° 50724280. 7,98 $/kg" — so 3,62 $ was announced as the price of a whole
// chicken when it is its price per pound. The banner's own words are lifted,
// verbatim, never invented: a line is taken only when it carries "/kg" or
// "/lb" next to a digit, which "moins de 2 kg" does not.
void liftWeightRate(model::Item& item);


class FlippSource final : public IFlyerSource
{
public:
    std::string id() const override { return "flipp"; }

    void setCanceller(http::Canceller* canceller) override { canceller_ = canceller; }

    Result fetchFlyers(const std::string& postalCode,
                       std::vector<model::Merchant>& merchantsOut,
                       std::vector<model::Flyer>& flyersOut,
                       const CancelFn& isCancelled) override;

    Result fetchItems(const model::Flyer& flyer,
                      const std::string& postalCode,
                      std::vector<model::Item>& itemsOut,
                      const CancelFn& isCancelled) override;

    Result fetchItemDetail(long long itemId,
                           const std::string& postalCode,
                           model::Item& itemInOut) override;

    Result search(const std::string& query,
                  const std::string& postalCode,
                  std::vector<model::Item>& itemsOut,
                  const CancelFn& isCancelled) override;

private:
    http::Canceller* canceller_ = nullptr;
};

} // namespace source
