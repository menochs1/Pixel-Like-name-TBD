// Derived materials, reaction heat, per-cell health, and chunked-vs-full
// equivalence under random edits.

#include <cmath>
#include <cstdlib>
#include <string>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "sandbox/tools.hpp"
#include "test_framework.hpp"

namespace {

using Op = px::Derivation::Op;

px::MaterialDef element(const char* name, px::Phase phase, float density, px::Color color, float conductivity) {
  px::MaterialDef d;
  d.name = name;
  d.phase = phase;
  d.density = density;
  d.color = color;
  d.conductivity = conductivity;
  return d;
}

/// alpha (dense, conductive, flammable) and beta (light, insulating-ish,
/// not flammable), and their product "blend" (a flammable liquid).
struct Chem {
  px::MaterialRegistry reg;
  px::MaterialId alpha, beta, blend, fire;
  Chem() {
    px::MaterialDef f = element("fire", px::Phase::Solid, 500, {255, 100, 0}, 0.9f);
    f.lifetime_min = f.lifetime_max = 30;
    fire = reg.add(f);
    px::MaterialDef a = element("alpha", px::Phase::Liquid, 2000, {200, 0, 0}, 0.8f);
    a.high_temp = 300;
    a.high_to = "fire";
    a.high_chance = 0.4f;
    alpha = reg.add(a);
    beta = reg.add(element("beta", px::Phase::Liquid, 1000, {0, 0, 200}, 0.2f));
    px::MaterialDef b = element("blend", px::Phase::Liquid, 1500, {50, 50, 50}, 0.5f);
    b.high_temp = 500;
    b.high_to = "fire";
    b.high_chance = 0.1f;
    blend = reg.add(b);
    reg.finalize();
  }
  static px::Derivation mean_all() {
    px::Derivation d;
    d.density = d.conductivity = d.color = d.ignition = d.flammability = {Op::Mean};
    return d;
  }
};

}  // namespace

TEST(derive_mixes_parent_properties_and_interns_the_result) {
  Chem c;
  const px::MaterialId v = c.reg.derive(c.blend, Chem::mean_all(), c.alpha, c.beta);
  CHECK(v != c.blend);
  CHECK_EQ(c.reg.derived_count(), std::size_t{1});
  const px::MaterialProps& p = c.reg.props(v);
  CHECK_EQ(p.base, c.blend);
  CHECK(std::fabs(p.density - 1500.0f) < 0.01f);        // (2000 + 1000) / 2
  CHECK(std::fabs(p.conductivity - 0.5f) < 0.011f);     // (0.8 + 0.2) / 2
  CHECK_EQ(c.reg.def(v).color.r, 100);
  CHECK_EQ(c.reg.def(v).color.b, 100);
  CHECK(p.has_high);
  CHECK_EQ(p.high_to, c.fire);       // behaviour comes from the product element
  CHECK_EQ(p.high_temp, 300);        // beta has no ignition point: alpha's is used
  CHECK(std::abs(p.high_chance - static_cast<int>(0.2f * 65535)) < 70);  // (0.4 + 0) / 2
  CHECK(c.reg.def(v).name.rfind("blend#", 0) == 0);
  // The same parents give the same material id, not a new one.
  CHECK_EQ(c.reg.derive(c.blend, Chem::mean_all(), c.alpha, c.beta), v);
  CHECK_EQ(c.reg.derived_count(), std::size_t{1});
}

TEST(derive_rules_pick_from_each_parent) {
  Chem c;
  px::Derivation d;
  d.density = {Op::A};
  d.conductivity = {Op::Max};
  d.color = {Op::Mix, 0.25f};
  const px::MaterialId v = c.reg.derive(c.blend, d, c.alpha, c.beta);
  CHECK(std::fabs(c.reg.props(v).density - 2000.0f) < 0.01f);
  CHECK(std::fabs(c.reg.props(v).conductivity - 0.8f) < 0.011f);
  CHECK_EQ(c.reg.def(v).color.r, 50);   // 0.25 * 200 + 0.75 * 0
  CHECK_EQ(c.reg.def(v).color.b, 150);  // 0.25 * 0 + 0.75 * 200
  CHECK_EQ(c.reg.props(v).high_temp, 500);  // no ignition rule: the product's own
  px::Derivation e;
  e.density = {Op::Min};
  CHECK(std::fabs(c.reg.props(c.reg.derive(c.blend, e, c.alpha, c.beta)).density - 1000.0f) < 0.01f);
}

TEST(derive_without_change_returns_the_element_itself) {
  Chem c;
  CHECK_EQ(c.reg.derive(c.blend, px::Derivation{}, c.alpha, c.beta), c.blend);  // every rule: Product
  CHECK_EQ(c.reg.derived_count(), std::size_t{0});
}

// Deriving again and again from derived parents converges on a few variants
// instead of minting one per reaction, because results are rounded.
TEST(chains_of_derivation_stay_bounded) {
  Chem c;
  px::MaterialId v = c.alpha;
  for (int i = 0; i < 500; ++i) v = c.reg.derive(c.blend, Chem::mean_all(), v, c.beta);
  CHECK(c.reg.derived_count() < 40);
  CHECK(std::fabs(c.reg.props(v).density - 1000.0f) < 20.0f);  // converged towards beta
}

TEST(registry_full_falls_back_to_the_element_and_keeps_references_valid) {
  Chem c;
  px::MaterialDef heavy = element("heavy", px::Phase::Liquid, 400000, {0, 0, 0}, 0.5f);
  px::MaterialDef light = element("light", px::Phase::Liquid, 0, {0, 0, 0}, 0.5f);
  px::MaterialRegistry& reg = c.reg;
  const px::MaterialId h = reg.add(heavy), l = reg.add(light);
  reg.finalize();
  const px::MaterialProps* before = &reg.props(c.alpha);
  int made = 0;
  for (int k = 0; k < static_cast<int>(px::kMaxMaterials) + 50; ++k) {
    px::Derivation d;
    d.density = {Op::Mix, static_cast<float>(k) / static_cast<float>(px::kMaxMaterials + 50)};
    made += reg.derive(c.blend, d, h, l) != c.blend ? 1 : 0;
  }
  CHECK_EQ(reg.size(), px::kMaxMaterials);
  CHECK(made > static_cast<int>(px::kMaxMaterials) - 100);
  CHECK(&reg.props(c.alpha) == before);  // the property table never moved
  px::Derivation last;
  last.density = {Op::Mix, 0.123456f};
  CHECK_EQ(reg.derive(c.blend, last, h, l), c.blend);  // full: the plain element
}

TEST(variants_react_like_their_base_element) {
  px::MaterialRegistry reg;
  reg.add(element("acid", px::Phase::Liquid, 1100, {100, 255, 0}, 0.5f));
  reg.add(element("rock", px::Phase::Solid, 2500, {90, 90, 90}, 0.3f));
  reg.add(element("salt", px::Phase::Powder, 1200, {240, 240, 240}, 0.3f));
  reg.add_reaction("acid", "rock", "salt", "empty", 1.0f);
  reg.finalize();
  const px::MaterialId acid = reg.require("acid"), rock = reg.require("rock");
  px::Derivation d;
  d.color = {Op::A};
  const px::MaterialId strong_acid = reg.derive(acid, d, rock, rock);  // grey acid, still acid
  CHECK(strong_acid != acid);
  CHECK(reg.reaction(strong_acid, rock) != nullptr);
  CHECK_EQ(reg.reaction(strong_acid, rock)->a_to, reg.require("salt"));
  CHECK(reg.props(strong_acid).has_reactions);
}

TEST(reactions_derive_products_and_release_heat) {
  px::MaterialRegistry reg;
  reg.add(element("hot", px::Phase::Solid, 1000, {255, 0, 0}, 0.5f));
  reg.add(element("cold", px::Phase::Solid, 3000, {0, 0, 255}, 0.5f));
  reg.add(element("ash", px::Phase::Solid, 500, {80, 80, 80}, 0.5f));
  px::ReactionDef r;
  r.a = "hot";
  r.b = "cold";
  r.a_to = "ash";
  r.b_to = "ash";
  r.heat = 300;
  px::Derivation d;
  d.density = {Op::Mix, 0.75f};
  r.a_derive = d;  // only the first product is derived
  reg.add_reaction(r);
  reg.finalize();
  px::World w(1, 1, reg, 3);
  w.set(10, 10, reg.require("hot"));
  w.set(11, 10, reg.require("cold"));
  px::step(w);
  const px::MaterialId a = w.get(10, 10), b = w.get(11, 10);
  CHECK_EQ(reg.props(a).base, reg.require("ash"));
  CHECK(std::fabs(reg.props(a).density - 1500.0f) < 0.01f);  // 0.75 * 1000 + 0.25 * 3000
  CHECK_EQ(b, reg.require("ash"));                             // not derived: plain ash
  CHECK(w.temperature(10, 10) > 250);                          // the reaction released heat
  CHECK(w.temperature(11, 10) > 250);
}

// The same reaction seen from the other side (the second-named reactant acting)
// must derive the same products.
TEST(derived_products_do_not_depend_on_which_cell_acted) {
  px::MaterialRegistry reg;
  reg.add(element("p", px::Phase::Solid, 1000, {255, 0, 0}, 0.5f));
  reg.add(element("q", px::Phase::Solid, 3000, {0, 0, 255}, 0.5f));
  reg.add(element("pq", px::Phase::Solid, 500, {80, 80, 80}, 0.5f));
  reg.add(element("qp", px::Phase::Solid, 500, {80, 80, 80}, 0.5f));
  px::ReactionDef r;
  r.a = "p";
  r.b = "q";
  r.a_to = "pq";
  r.b_to = "qp";
  px::Derivation d;
  d.density = {Op::Mix, 0.9f};  // 90% from p
  r.a_derive = d;
  r.b_derive = d;
  reg.add_reaction(r);
  reg.finalize();
  const px::Reaction* from_p = reg.reaction(reg.require("p"), reg.require("q"));
  const px::Reaction* from_q = reg.reaction(reg.require("q"), reg.require("p"));
  CHECK(!from_p->swapped);
  CHECK(from_q->swapped);
  CHECK_EQ(from_q->a_to, reg.require("qp"));  // q's own product
  px::World w(1, 1, reg, 3);
  w.set(10, 10, reg.require("q"));  // q left of p this time
  w.set(11, 10, reg.require("p"));
  px::step(w);
  // Whichever cell acted, p's cell became pq and q's became qp, both 90% p.
  CHECK_EQ(reg.props(w.get(11, 10)).base, reg.require("pq"));
  CHECK_EQ(reg.props(w.get(10, 10)).base, reg.require("qp"));
  CHECK(std::fabs(reg.props(w.get(11, 10)).density - 1200.0f) < 0.01f);
  CHECK(std::fabs(reg.props(w.get(10, 10)).density - 1200.0f) < 0.01f);
}

TEST(oil_soaks_into_sand_and_makes_tar) {
  px::MaterialRegistry reg;
  const px::BaseMaterials ids = px::register_base_materials(reg);
  px::World w(1, 1, reg, 21);
  w.fill_rect(0, 60, 64, 4, ids.stone);
  w.fill_rect(0, 50, 64, 10, ids.sand);
  w.fill_rect(0, 44, 64, 6, ids.oil);
  for (int i = 0; i < 600; ++i) px::step(w);
  int tar = 0;
  px::MaterialId some_tar = px::kEmpty;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x)
      if (reg.props(w.get(x, y)).base == ids.tar) {
        ++tar;
        some_tar = w.get(x, y);
      }
  CHECK(tar > 50);
  CHECK(some_tar != ids.tar);  // a derived variant, not the fallback element
  const px::MaterialProps& p = reg.props(some_tar);
  CHECK(std::fabs(p.density - 1220.0f) < 10.1f);  // mean of oil (850) and sand (1600)
  CHECK(p.density > reg.props(ids.oil).density);  // so it sinks through oil
  CHECK_EQ(p.high_temp, 200);                      // oil's ignition point
  CHECK(p.high_chance < reg.props(ids.oil).high_chance);  // diluted by the sand
  CHECK(p.high_chance > 0);
}

TEST(damage_wears_cells_down_until_they_break) {
  px::MaterialRegistry reg;
  const px::BaseMaterials ids = px::register_base_materials(reg);  // stone durability 200
  px::World w(1, 1, reg, 4);
  w.set(5, 5, ids.stone);
  CHECK_EQ(w.health(5, 5), 255);
  CHECK(!w.damage(5, 5, 100));
  CHECK(w.health(5, 5) < 130);
  CHECK(w.health(5, 5) > 120);
  CHECK(w.damage(5, 5, 100));  // the second hit breaks it
  CHECK_EQ(w.get(5, 5), px::kEmpty);
  CHECK(!w.damage(5, 5, 100));  // nothing left to break
  w.set(6, 6, ids.water);       // durability 0: any damage breaks it
  CHECK(w.damage(6, 6, 1));

  // Health travels with a moving cell and resets when the material changes.
  w.set(20, 10, ids.sand);  // durability 10
  w.damage(20, 10, 5);
  const int worn = w.health(20, 10);
  CHECK(worn < 255);
  for (int i = 0; i < 100; ++i) px::step(w);
  CHECK_EQ(w.get(20, 63), ids.sand);
  CHECK_EQ(w.health(20, 63), worn);
  w.set(20, 63, ids.stone);
  CHECK_EQ(w.health(20, 63), 255);
}

// Random painting of every material, random temperature spikes and heat/cool
// brushes, on walled
// and wrapping worlds: skipping sleeping cells must give exactly the same world
// as updating every cell. This found a heat-exchange cell that slept through
// its turn.
TEST(chunked_simulation_matches_full_updates_under_random_edits) {
  for (int seed = 1; seed <= 6; ++seed) {
    px::MaterialRegistry m;
    const px::BaseMaterials ids = px::register_base_materials(m);
    const px::MaterialId pal[] = {ids.sand, ids.water, ids.oil,    ids.fire,  ids.lava,  ids.ice,   ids.wood,
                                  ids.steam, ids.coal, ids.leaves, ids.tar, px::kEmpty, ids.stone, ids.basalt};
    const px::EdgeX edge = seed % 2 ? px::EdgeX::Wrap : px::EdgeX::Wall;
    px::World a(3, 2, m, static_cast<std::uint64_t>(seed), edge), b(3, 2, m, static_cast<std::uint64_t>(seed), edge);
    std::uint32_t r = static_cast<std::uint32_t>(seed) * 7919U;
    auto rnd = [&](int n) {
      r = px::hash32(r + 0x9E3779B9U);
      return static_cast<int>(r % static_cast<std::uint32_t>(n));
    };
    for (px::World* w : {&a, &b}) w->fill_rect(0, w->height() - 4, w->width(), 4, ids.stone);
    bool same = true;
    for (int s = 1; s <= 300 && same; ++s) {
      if (s % 10 == 1) {
        const int x = rnd(a.width() + 40) - 20, y = rnd(a.height()), rad = 1 + rnd(8);
        const px::MaterialId mat = pal[rnd(14)];
        const float cov = rnd(2) ? 1.0f : 0.55f;
        a.fill_circle(x, y, rad, mat, cov);
        b.fill_circle(x, y, rad, mat, cov);
        if (rnd(4) == 0) {
          const int tx = rnd(a.width()), ty = rnd(a.height()), t = rnd(1500) - 100;
          a.set_temperature(tx, ty, t);
          b.set_temperature(tx, ty, t);
        }
      }
      if (s % 50 < 15) {  // a heat or cool brush held for a while (melting, quenching)
        const int bx = (seed * 37 + s / 50 * 53) % a.width(), by = 30 + (s / 50 * 17) % 60;
        const px::ThermalBrush& brush = (s / 50) % 2 ? px::kCoolBrush : px::kHeatBrush;
        px::apply_thermal_brush(a, bx, by, 6, brush);
        px::apply_thermal_brush(b, bx, by, 6, brush);
      }
      px::step(a);
      b.wake_all();
      px::step(b);
      for (int y = 0; y < a.height() && same; ++y)
        for (int x = 0; x < a.width() && same; ++x)
          same = a.get(x, y) == b.get(x, y) && a.temperature(x, y) == b.temperature(x, y);
    }
    CHECK(same);
  }
}
