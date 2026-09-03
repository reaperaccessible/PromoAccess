# PromoAccess

**Les circulaires d'épicerie du Québec, accessibles au lecteur d'écran.**
*Quebec grocery flyers, readable with a screen reader.*

Par [ReaperAccessible](https://reaperaccessible.fr) — logiciels accessibles pour
personnes non-voyantes.

---

## Français

PromoAccess télécharge les circulaires hebdomadaires des bannières d'épicerie du
Québec — IGA, Metro, Super C, Maxi, Costco, Les Marchés Tradition et une
quarantaine d'autres — et les rend entièrement utilisables au clavier avec NVDA
ou JAWS.

### Ce qu'il fait

- **Circulaires** — les livrets des bannières que vous suivez, article par
  article, avec le prix, le format et la date de fin.
- **Recherche** — un mot, et tous les rabais de toutes les circulaires à la
  fois. Instantané, hors ligne, sans accent obligatoire.
- **Favoris** — une règle de mots, pas un produit épinglé : les articles
  changent d'identifiant chaque semaine, alors PromoAccess reteste vos favoris à
  chaque synchronisation et vous dit ce qui est en rabais en ce moment.
- **Liste d'épicerie** — ajoutez, ajustez les quantités, exportez en texte, CSV
  ou Markdown.
- **Détails d'un article** — un champ que le lecteur d'écran parcourt ligne par
  ligne : prix initial, économie, format, message promotionnel, lien vers la
  fiche du produit.

Tout est conservé sur votre ordinateur. Après une synchronisation, la
navigation, la recherche et la liste fonctionnent sans connexion.

### Accessibilité

L'accessibilité n'est pas une couche ajoutée à la fin, c'est la contrainte de
départ. Chaque liste annonce sa ligne, chaque champ porte une étiquette réelle,
le nom de l'onglet est énoncé avant le contrôle, et les annonces sont
télégraphiques plutôt que bavardes. Cinq onglets, `Ctrl+1` à `Ctrl+5`, et rien
qui exige la souris.

Le manuel complet est dans [Docs/manual_fr.html](Docs/manual_fr.html), et
`F1` l'ouvre depuis l'application.

### Installation

Téléchargez l'installeur depuis la page des
[versions](https://github.com/reaperaccessible/PromoAccess/releases), ou
compilez-le vous-même — voir plus bas.

---

## English

PromoAccess downloads the weekly flyers of Quebec grocery banners — IGA, Metro,
Super C, Maxi, Costco and some forty others — and makes them fully usable from
the keyboard with NVDA or JAWS.

### What it does

- **Flyers** — the books of the banners you follow, item by item, with the
  price, the format and the closing date.
- **Search** — one word, and every deal in every flyer at once. Instant,
  offline, accent-insensitive.
- **Favorites** — a rule made of words, not a pinned product: flyer items get
  new identifiers every week, so PromoAccess re-tests your favorites at every
  sync and tells you what is on sale right now.
- **Shopping list** — add, adjust quantities, export to text, CSV or Markdown.
- **Item details** — a field the screen reader walks line by line: original
  price, saving, format, sale story, link to the product page.

Everything stays on your computer. After a sync, browsing, searching and list
building work with no connection at all.

### Accessibility

Accessibility is not a layer added at the end here, it is the starting
constraint. Every list announces its row, every field carries a real label, the
tab name is spoken before the control, and announcements are terse rather than
chatty. Five tabs, `Ctrl+1` to `Ctrl+5`, and nothing that needs a mouse.

The full manual is in [Docs/manual_en.html](Docs/manual_en.html), and `F1`
opens it from inside the application.

---

## Compiler / Building

Windows, Visual Studio 2022, CMake 3.22 or newer.

Dependencies come from [vcpkg](https://vcpkg.io) with the
`x64-windows-static` triplet:

```
vcpkg install wxwidgets:x64-windows-static nlohmann-json:x64-windows-static
```

Then:

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

SQLite is vendored in `Source/ThirdParty/sqlite`, so there is nothing else to
fetch. The build is static: the executable imports only Windows system
libraries, and there is no runtime to redistribute.

`Source/Tools/PromoTest.cpp` builds alongside it as `promotest`. Run it with no
argument for the offline checks, or with a postal code — `promotest J3P7S7` —
to add a live sync and search against the real service. Its exit code is the
number of failed checks.

The installer is built with [Inno Setup 6](https://jrsoftware.org/isinfo.php):

```
ISCC.exe /DMyAppVersion=1.00 /DSourceDir=<repo> /DOutputDir=<repo>\Output installer.iss
```

## Licence

[GPL-3.0](LICENSE), comme les autres logiciels de ReaperAccessible.

Les données des circulaires viennent de Flipp / Wishabi. Les noms de villes des
codes postaux viennent des données postales
[GeoNames](https://download.geonames.org/export/zip/), sous licence Creative
Commons Attribution 4.0. Les noms de produits, les prix et les images
appartiennent aux bannières qui les publient.
