// SPDX-License-Identifier: GPL-3.0-or-later
// Console du plugin decklink_io — chantier docs/chantiers/DECKLINK.md.
//
// ⚠ CONTRAT DU SHELL : il charge le fragment HTML, puis ce script, puis appelle
// `window.MXLPlugins[type].mount(el, vmid, ctx)`. Sans cet enregistrement il remplace le
// contenu par « Ce plugin n'embarque pas d'UI de contrôle ».
//
// ★ POURQUOI UN DESSIN. Rien n'est étiqueté sur une DeckLink, et le SDK ne donne AUCUNE
//   position physique : il rend des sous-périphériques, pas des prises sur une équerre. Ce
//   panneau montre donc ce que la carte SAIT (rôle, duplex, signal) dans l'ordre où elle
//   l'énumère, et il dit que cet ordre n'est pas vérifié. Inventer une correspondance qu'on
//   n'a pas serait pire que de ne rien dessiner : l'exploitant se brancherait avec confiance
//   au mauvais endroit. Le 2026-09-09, une demi-journée s'est perdue exactement là-dessus.
window.MXLPlugins = window.MXLPlugins || {};
window.MXLPlugins.decklink_io = (function () {
  "use strict";

  const T = (k, fb) => {
    const c = "plugin.decklink_io." + k;
    const v = window.t ? window.t(c) : "";
    return (v && v !== c) ? v : fb;
  };

  // Libellés du CŒUR (pilote hôte `blackmagic`) : ses codes sont traduits par le catalogue
  // central, pas par celui du plugin — T() ne les atteint pas, il préfixe `plugin.decklink_io.`.
  const TC = (k, fb) => {
    const v = window.t ? window.t(k) : "";
    return (v && v !== k) ? v : fb;
  };

  // Toutes les URL passent par ici — une seule fonction, jamais un `if` par appel : c'est ce
  // qui permet à la même page de servir derrière un jeton public sans 401 dispersés.
  let BASE = null, VMID = null, EL = null, minuteur = 0;
  const api = (chemin) => BASE ? (BASE + "/plugin" + chemin)
                               : ("/api/containers/" + VMID + "/plugin" + chemin);

  const PERIODE_MS = 2000;      // l'inventaire est mis en cache 5 s côté conteneur
  let inventaire = null, etat = null;
  // ── Édition ────────────────────────────────────────────────────────────────────────────
  // `config` est la configuration PERSISTÉE (plugin_config), pas ce que le conteneur rend :
  // /state est une photo prise au déploiement, pas un miroir. Se reconstruire sur la photo
  // reviendrait à défaire un réglage qu'on vient d'enregistrer.
  let config = null;            // {inputs:[…], outputs:[…]} tels qu'en base
  // ★ CHANGEMENTS EN LOT (07/11, demandé par l'utilisateur). Les cœurs de la carte suivent ses prises
  //   actives : activer une prise recrée le conteneur. On prépare donc plusieurs prises, puis on
  //   applique une seule fois — un redémarrage pour le lot, pas un par prise. `lot` = les deux listes
  //   voulues ; `lotOrigine` = la configuration d'avant, pour l'abandon et le compte des changements.
  let lot = null;
  let lotOrigine = null;
  let choisi = null;            // persistent_id en cours d'édition
  let envoiEnCours = false;
  let aConfirmer = false;       // le serveur a réclamé un consentement explicite
  let plafondAccepte = false;   // l'exploitant a vu l'avertissement de plafond et insiste
  // ★★ PLAFOND MESURÉ : 5 SORTIES SIMULTANÉES PAR CARTE (23/10). Au-delà, des sorties passent
  //    en retard permanent et la carte ÉCARTE LEUR AUDIO — l'image reste, le son disparaît, et
  //    aucun compteur ne bronche : 50 i/s et genlock verrouillé sur toutes. Le testeur a
  //    cherché la panne une journée entière dans ses câbles, ses canaux et ses flux.
  // ⚠⚠ C'EST LE COMPTE PAR CARTE, PAS LE TOTAL — et j'ai d'abord publié le total, à tort.
  //    Mesures sur une Quad 2 + une Duo 2, 3 entrées actives, tout en 1080p50 :
  //      Quad 4 + Duo 0 = 4  → ok          Quad 6 + Duo 2 = 8  → ÉCHEC
  //      Quad 4 + Duo 2 = 6  → ok          Quad 7 + Duo 0 = 7  → ÉCHEC
  //      Quad 5 + Duo 2 = 7  → ok          Quad 7 + Duo 2 = 9  → ÉCHEC
  //    « 7 au total » décrivait donc une RÉPARTITION (5+2) prise pour une limite : sept sorties
  //    toutes sur la Quad échouent, six réparties passent. Le total ne prédit rien.
  //    Indépendant du connecteur, du processeur (6,6 cœurs sur 45, aucun au-dessus de 90 %) et
  //    de la DURÉE (17 h 24 continues à 4 sorties : pas une seule trame tardive).
  // ⚠ UNE MESURE, PAS UNE SPÉCIFICATION : la cause n'est pas isolée, donc ce chiffre peut
  //   bouger avec le format ou le nombre d'entrées, et il n'est VÉRIFIÉ que sur la Quad 2.
  //   D'où un avertissement, et jamais un refus.
  const PLAFOND_SORTIES_PAR_CARTE = 5;

  /// Cartes dont le nombre de sorties déclarées dépasse le plafond : [{carte, n}].
  function cartesEnSurcharge(sorties) {
    const parCarte = new Map();
    sorties.forEach((o) => {
      const c = (o && o.carte) || "";
      parCarte.set(c, (parCarte.get(c) || 0) + 1);
    });
    const out = [];
    parCarte.forEach((n, c) => {
      if (n > PLAFOND_SORTIES_PAR_CARTE) out.push({ carte: c, n: n });
    });
    return out;
  }
  // ★ CE QU'ON VIENT D'ENVOYER FAIT FOI TANT QUE LA BASE NE L'A PAS ÉCRIT (21/10, signalé en
  //   recette). Le serveur répond tout de suite, mais n'écrit la configuration qu'au BOUT du
  //   redéploiement — 40 à 80 s mesurées ce matin. La page la relisait toutes les 2 s, retrouvait
  //   l'ancienne et ré-affichait l'ancien rôle : une prise qu'on venait de passer en sortie
  //   redevenait « entrée » sous les yeux de l'exploitant, qui rechargeait la page pour y croire.
  //   Pire, un second réglage fait dans cet intervalle partait de l'ancienne liste et DÉFAISAIT
  //   le premier. On garde donc l'envoi jusqu'à ce que la base le rende — avec une limite :
  //   passé ce délai la base reprend la main, un redéploiement échoué ne doit pas faire mentir
  //   la page.
  let enAttente = null;         // {sig, inputs, outputs, depuis} : envoyé, pas encore relu
  const ATTENTE_MAX_MS = 180000;
  const editable = () => !BASE;  // mode public (jeton) = lecture seule, jamais de POST

  const cfgApi = (chemin) => "/api/containers/" + VMID + chemin;

  async function lire(chemin) {
    const r = await fetch(api(chemin), { credentials: "same-origin" });
    if (!r.ok) throw new Error(chemin + " → " + r.status);
    return r.json();
  }

  // ── Ce que la carte dit d'une prise ────────────────────────────────────────────────────
  // Le rôle n'est PAS une propriété de la carte : un sous-périphérique half-duplex est entrée
  // OU sortie selon ce qu'on ouvre. On le déduit donc de la configuration DÉPLOYÉE, et on
  // retombe sur « libre » quand personne ne s'en sert — plutôt que d'inventer une direction.
  function role(sub, cfg) {
    if ((sub.duplex || "") === "inactive") return "inactif";
    if (cfg.entrees.has(sub.persistent_id)) return "entree";
    if (cfg.decoupes.has(sub.persistent_id)) return "decoupe";
    if (cfg.sorties.has(sub.persistent_id)) return "sortie";
    return "libre";
  }

  const LIBELLE_ROLE = {
    entree:  () => T("role.entree", "entrée"),
    sortie:  () => T("role.sortie", "sortie"),
    inactif: () => T("role.inactif", "INACTIVE"),
    decoupe: () => T("role.decoupe", "découpe"),
    libre:   () => T("role.libre", "libre"),
    ref:     () => T("role.ref", "référence"),
  };

  function configDeployee(st) {
    const entrees = new Map(), sorties = new Map();
    const src = config || st;            // la base fait foi ; /state n'est qu'un repli
    ((src && src.inputs) || []).forEach((e) => {
      if (e && e.persistent_id != null) entrees.set(Number(e.persistent_id), e);
    });
    const decoupes = new Map();
    ((src && src.outputs) || []).forEach((o) => {
      if (!o) return;
      if (o.persistent_id != null) sorties.set(Number(o.persistent_id), o);
      // La découpe occupe un connecteur sans être une sortie à part entière : elle appartient
      // à la sortie qui la déclare, et c'est ce lien qu'il faut montrer.
      if (o.key_persistent_id) decoupes.set(Number(o.key_persistent_id), o);
    });
    return { entrees, sorties, decoupes };
  }

  function sousPeripheriques(inv) {
    if (!inv) return [];
    // La sonde rend son tableau sous une clé qu'on ne fige pas ici : on prend le premier
    // tableau d'objets portant un persistent_id. Coder le nom de la clé en dur casserait au
    // premier renommage de la sonde, sans rien dire.
    for (const v of Object.values(inv)) {
      if (Array.isArray(v) && v.length && v[0] && v[0].persistent_id != null) return ordrePhysique(v);
    }
    return [];
  }

  // Le nom que l'exploitant lit PARTOUT, calculé à UN SEUL endroit : le rang dans le bandeau.
  // Deux calculs séparés donnent deux numéros différents pour la même prise — c'est ce qui est
  // arrivé le 19/10, la pastille portant la position et l'étiquette le nom du SDK.
  // Le bandeau suivant l'ordre d'ÉNUMÉRATION (cf. plus bas), ce rang coïncide avec le numéro
  // que la carte donne elle-même — « SDI 2 » et « DeckLink Duo (2) » désignent bien la même.
  // ★ LE NUMÉRO REPART À 1 SUR CHAQUE CARTE (20/10). Il était calculé sur le rang GLOBAL : avec
  //   deux cartes dans la machine, la Duo 2 héritait des numéros 9 à 12 — qui ne correspondent à
  //   rien de ce qui est écrit sur son équerre, et à rien de ce que l'exploitant peut compter du
  //   doigt. Le numéro est une POSITION SUR UNE CARTE, pas un rang dans une énumération.
  //   Quand il y a plusieurs cartes, on préfixe par le modèle, sinon le « SDI 1 » de l'une ne se
  //   distingue pas du « SDI 1 » de l'autre dans une liste déroulante.
  function nomPhysique(pid) {
    const groupes = grouperParCarte(sousPeripheriques(inventaire));
    for (const g of groupes) {
      const i = g.findIndex((x) => Number(x.persistent_id) === Number(pid));
      if (i < 0) continue;
      const nom = T("prise.etiquette", "SDI") + " " + (i + 1);
      return groupes.length > 1 ? ((g[0] && g[0].model ? g[0].model + " — " : "") + nom) : nom;
    }
    return String(pid);
  }

  // ★★ LES PRISES SONT DESSINÉES DANS L'ORDRE PHYSIQUE DE L'ÉQUERRE (2026-10-19).
  //    Le SDK ÉNUMÈRE les sous-périphériques dans un ordre qui n'est pas celui des BNC : les
  //    deux moitiés de la carte sont ENTRELACÉES. Deux sources indépendantes le disent, et
  //    elles concordent exactement :
  //      1. l'ordre d'équerre d'une Quad 2, donné par l'exploitant : Ref, 1, 5, 2, 6, 3, 7, 4, 8
  //         — soit, pour une carte de quatre connecteurs, la suite 1, 3, 2, 4 ;
  //      2. `BMDDeckLinkTopologicalID` relevé sur la Duo 2 : 696, 698, 697, 699, qui trié donne
  //         exactement ce même 1, 3, 2, 4.
  //    Le constructeur et la carte, jamais confrontés, disent la même chose.
  //
  //    ⚠ CE TRI A ÉTÉ POSÉ, RETIRÉ, PUIS REMIS LE MÊME JOUR. Il avait été retiré parce que
  //      l'exploitant avait corrigé une information — mais sa correction portait sur l'endroit
  //      où étaient ses CÂBLES, pas sur l'ordre. Entre-temps j'avais aussi conclu trop vite à
  //      partir de deux débranchements aux EXTRÉMITÉS, que cette permutation épargne
  //      précisément. Vérifier les bords ne vérifie pas le milieu.
  //
  //    Trier par position déclarée vaut pour N'IMPORTE QUELLE carte — aucune branche par modèle,
  //    ce qui est la règle du chantier, et la Quad 2 en bénéficiera sans rien coder. Une carte
  //    qui ne rapporterait pas ce champ garde l'ordre d'énumération, faute de mieux.
  function ordrePhysique(subs) {
    const complet = subs.every((s) => s && typeof s.topological_id === "number");
    if (!complet) return subs;
    return subs.slice().sort((a, b) => a.topological_id - b.topological_id);
  }

  // ── Le bandeau ─────────────────────────────────────────────────────────────────────────
  // ── NOM LISIBLE D'UN MODE VIDÉO ───────────────────────────────────────────────────────────
  // La carte rend le mode détecté sous forme de code à quatre caractères (« Hp50 »), qui ne dit
  // rien à un exploitant. Cette table est GÉNÉRÉE depuis `decklink_sdk/DeckLinkAPIModes.h` (les
  // noms d'énumération y portent le format) — elle n'est pas écrite à la main, donc elle ne peut
  // pas inventer un format que le SDK ne nomme pas. Un code absent est affiché TEL QUEL : un
  // code brut reste une piste, un tiret n'en est pas une.
  const MODES_SDK = {
    "1945":"1920x1440p50", "1946":"1920x1440p60", "23ps":"1080p23.98", "24ps":"1080p24",
    "2d10":"2kDCI100", "2d11":"2kDCI11988", "2d12":"2kDCI120", "2d23":"2kDCI2398",
    "2d24":"2kDCI24", "2d25":"2kDCI25", "2d29":"2kDCI2997", "2d30":"2kDCI30",
    "2d47":"2kDCI4795", "2d48":"2kDCI48", "2d50":"2kDCI50", "2d59":"2kDCI5994",
    "2d60":"2kDCI60", "2d95":"2kDCI9590", "2d96":"2kDCI96", "2k23":"2k2398", "2k24":"2k24",
    "2k25":"2k25", "4d10":"4kDCI100", "4d11":"4kDCI11988", "4d12":"4kDCI120",
    "4d23":"4kDCI2398", "4d24":"4kDCI24", "4d25":"4kDCI25", "4d29":"4kDCI2997",
    "4d30":"4kDCI30", "4d47":"4kDCI4795", "4d48":"4kDCI48", "4d50":"4kDCI50",
    "4d59":"4kDCI5994", "4d60":"4kDCI60", "4d95":"4kDCI9590", "4d96":"4kDCI96",
    "4k10":"4K2160p100", "4k11":"4K2160p11988", "4k12":"4K2160p120", "4k23":"4K2160p23.98",
    "4k24":"4K2160p24", "4k25":"4K2160p25", "4k29":"4K2160p29.97", "4k30":"4K2160p30",
    "4k47":"4K2160p47.95", "4k48":"4K2160p48", "4k50":"4K2160p50", "4k59":"4K2160p59.94",
    "4k60":"4K2160p60", "4k95":"4K2160p95.9", "4k96":"4K2160p96", "8d23":"8kDCI2398",
    "8d24":"8kDCI24", "8d25":"8kDCI25", "8d29":"8kDCI2997", "8d30":"8kDCI30",
    "8d47":"8kDCI4795", "8d48":"8kDCI48", "8d50":"8kDCI50", "8d59":"8kDCI5994",
    "8d60":"8kDCI60", "8k23":"8K4320p23.98", "8k24":"8K4320p24", "8k25":"8K4320p25",
    "8k29":"8K4320p29.97", "8k30":"8K4320p30", "8k47":"8K4320p47.95", "8k48":"8K4320p48",
    "8k50":"8K4320p50", "8k59":"8K4320p59.94", "8k60":"8K4320p60", "Hi50":"1080i50",
    "Hi59":"1080i59.94", "Hi60":"1080i60", "Hp10":"1080p100", "Hp11":"1080p11988",
    "Hp12":"1080p120", "Hp25":"1080p25", "Hp29":"1080p29.97", "Hp30":"1080p30",
    "Hp47":"1080p47.95", "Hp48":"1080p48", "Hp50":"1080p50", "Hp59":"1080p59.94",
    "Hp60":"1080p60", "Hp95":"1080p95.9", "Hp96":"1080p96", "hp50":"720p50", "hp59":"720p59.94",
    "hp60":"720p60", "nt23":"NTSC2398", "ntsc":"NTSC", "ntsp":"NTSCp", "pal ":"PAL",
    "palp":"PALp", "svg6":"800x600p60", "sxg5":"1440x1080p50", "sxg6":"1440x1080p60",
    "uxg5":"1600x1200p50", "uxg6":"1600x1200p60", "vga6":"640x480p60", "wqh5":"2560x1440p50",
    "wqh6":"2560x1440p60", "wqx5":"2560x1600p50", "wqx6":"2560x1600p60", "wux5":"1920x1200p50",
    "wux6":"1920x1200p60", "wxg5":"1440x900p50", "wxg6":"1440x900p60"
  };
  const nomDeMode = (code) => MODES_SDK[String(code || "")] || String(code || "");

  // Le format de la RÉFÉRENCE, que la carte nomme elle-même. Ajouté le 19/10 : deux testeurs ont
  // demandé le même jour « qu'est-ce qu'on reçoit comme ref ? » et nous ne lisions que le
  // drapeau « verrouillée », donc nous ne pouvions pas répondre.
  function formatReference(subs) {
    const s = (subs || []).find((x) => x.signal && x.signal.mode_reference);
    if (s) return nomDeMode(s.signal.mode_reference);
    return subs.some((x) => x.signal && x.signal.reference_verrouillee)
      ? T("ref.format_inconnu", "format inconnu") : "";
  }

  // ⚠ CE QUI EST AFFICHÉ SOUS CHAQUE PRISE. « Signal présent » ne suffit pas : on a mesuré un
  //   sous-périphérique qui l'annonçait avec 0 i/s et 0×0. On montre donc la CADENCE et le
  //   FORMAT, qui ne peuvent pas mentir de concert. Et sur une prise LIBRE, on montre quand même
  //   ce que la carte détecte : c'est ce qui dit « un câble vivant est branché ici » avant même
  //   de l'affecter — demandé le 19/10. L'absence de signal se DIT, elle ne se devine pas à un
  //   tiret.
  function mesurePrise(sub, vivant, r, modeSortie) {
    if (r === "inactif") return T("mesure.inactive", "éteinte par le profil");
    if (vivant && (vivant.fps || vivant.width)) {
      // ★ LE BALAYAGE (i ou p) EST AFFICHÉ, demandé le 19/10. « 1920×1080 » ne dit pas si on
      //   reçoit de l'entrelacé ou du progressif — or c'est ce qui décide du traitement en
      //   aval, et une erreur dessus produit un peigne ou une cadence divisée par deux. Le nom
      //   que donne la carte le porte déjà (« 1080p50 », « 1080i50ard ») : on l'affiche tel
      //   quel plutôt que de le recalculer, et on garde la cadence MESURÉE à côté, qui est la
      //   seule à pouvoir démentir le format annoncé.
      // ★ POUR UNE SORTIE, LE FORMAT EST CELUI QU'ON ÉMET, et il ne vient pas de la carte :
      //   elle ne DÉTECTE rien sur un connecteur qu'elle pilote. C'est le mode CONFIGURÉ qui
      //   fait foi — demandé le 19/10 : « 1920×1080 » ne disait pas si on émettait de
      //   l'entrelacé ou du progressif, or c'est le premier chiffre qu'un exploitant vérifie
      //   quand un équipement aval refuse le signal.
      const det0 = (r === "sortie" || r === "decoupe")
        ? (modeSortie || "")
        : (sub && sub.signal && sub.signal.mode_detecte ? nomDeMode(sub.signal.mode_detecte) : "");
      const tete = det0 || ((vivant.width || 0) + "×" + (vivant.height || 0));
      return tete + " · " + (Number(vivant.fps || 0)).toFixed(2) + " i/s";
    }
    // ⚠ « AUCUN SIGNAL » EST UNE NOTION D'ENTRÉE, ET L'APPLIQUER À UNE SORTIE EST UN FAUX
    //   DIAGNOSTIC. Une sortie qui n'émet pas encore affichait « aucun signal » — ce qui se lit
    //   comme une panne de réception sur une prise qui n'a rien à recevoir. Et la carte ne
    //   rapporte RIEN sur un connecteur configuré en sortie : mesuré le 19/10, un câble source
    //   branché sur une prise en sortie reste totalement invisible (mode_detecte null,
    //   entree_verrouillee false). On ne peut donc pas le détecter — on peut seulement éviter
    //   d'affirmer le contraire.
    if (r === "sortie" || r === "decoupe") {
      return T("mesure.sortie_attente", "n'émet pas encore");
    }
    const det = sub && sub.signal && sub.signal.mode_detecte;
    if (det) return nomDeMode(det) + " " + T("mesure.detecte", "détecté");
    return T("mesure.aucun_signal", "aucun signal");
  }

  // ── UN BANDEAU PAR CARTE ─────────────────────────────────────────────────────────────
  // ★ DEMANDÉ LE 20/10, le jour où une deuxième carte est apparue dans la machine. Les douze
  //   prises se suivaient dans un seul ruban : une Duo 2 et une Quad 2 bout à bout, avec deux
  //   numérotations qui recommencent à 1 et une seule référence dessinée pour les deux. Un
  //   exploitant qui cherche « la prise 3 » ne peut pas savoir de quelle carte il s'agit.
  // ★ LE DÉCOUPAGE VIENT DU SDK, pas d'une devinette sur les identifiants. Chaque
  //   sous-périphérique annonce son rang DANS SA CARTE (`sub_device_index`) et le nombre total
  //   de la carte (`sub_device_count`) : une carte est donc une suite dont les rangs forment
  //   exactement 0…count-1. Aucune hypothèse sur la contiguïté des `persistent_id`, aucune
  //   branche par modèle.
  function grouperParCarte(subs) {
    const groupes = [];
    let courant = [];
    subs.forEach((s) => {
      courant.push(s);
      const n = Number(s.sub_device_count || 0);
      // La carte est complète quand on tient autant de prises qu'elle en annonce ET que leurs
      // rangs sont tous distincts et couvrent 0…n-1. Sans cette seconde condition, deux cartes
      // du même modèle mises bout à bout se fondraient en une seule.
      if (n > 0 && courant.length === n) {
        const rangs = courant.map((x) => Number(x.sub_device_index));
        const complet = rangs.every((r) => r >= 0 && r < n)
                        && new Set(rangs).size === n;
        if (complet) { groupes.push(courant); courant = []; }
      }
    });
    if (courant.length) groupes.push(courant);   // relevé incohérent : on montre quand même
    return groupes;
  }

  // ── DISPOSITIONS ENREGISTRÉES (20/10, demandé en recette) ─────────────────────────────
  // Une « disposition » retient l'AFFECTATION des connecteurs d'une carte — entrée, sortie ou
  // libre, le format d'une sortie, sa pré-charge, l'appairage Fill & Key — et rien d'autre.
  //
  // ★ ELLE NE RETIENT PAS LES FLUX, délibérément. Un flux est du CÂBLAGE, et c'est la page
  //   Câbles qui en a la charge. Les embarquer ici ferait un second endroit où écrire la même
  //   valeur — la faute exacte qui, ce mois-ci, a produit une sortie qui se taisait sans rien
  //   dire. Au rappel, un connecteur qui GARDE son rôle garde donc le flux qu'il avait.
  // ★ MAIS ELLE RETIENT LE FORMAT D'UNE SORTIE, parce qu'il n'a pas de défaut acceptable : une
  //   sortie sans format est écartée au déploiement. Rappeler une disposition sans les formats
  //   ferait disparaître les sorties — un « rappel » qui efface n'est pas un rappel.
  function cleCarte(g) { return String((g[0] && g[0].persistent_id) || ""); }

  function dispositionsDe(cle) {
    const tout = (config && config.layouts) || {};
    return tout[cle] || {};
  }

  //: Zones « dispositions » déjà construites, par carte. ★★ ELLES NE SE RECONSTRUISENT PAS À
  //: CHAQUE REDESSIN (20/10, signalé en recette dans la minute) : le bandeau est refait toutes
  //: les 2 s, et recréer un <select> et un champ de saisie à cette cadence les REPLIE sous les
  //: doigts — liste déroulante qui se referme, texte en cours de frappe effacé. On construit
  //: UNE fois par carte, on DÉPLACE l'élément existant au redessin, et on ne met à jour que la
  //: liste des noms, et seulement si elle a changé.
  //: C'est la troisième fois aujourd'hui que le redessin détruit un élément à état. La règle
  //: vaut pour tous : un <select>, un champ de saisie, un repli, un onglet.
  const zonesDisp = new Map();

  function dessinerDispositions(bloc, g) {
    if (!editable()) return;
    const cle = cleCarte(g);
    const noms = Object.keys(dispositionsDe(cle)).sort();

    const dejaLa = zonesDisp.get(cle);
    if (dejaLa) {
      // ⚠ ON NE RÉ-ATTACHE QUE SI C'EST NÉCESSAIRE. `appendChild` sur un élément DÉJÀ au bon
      //   endroit le détache et le rattache quand même : ça referme la liste déroulante
      //   ouverte et interrompt la saisie en cours. Fait toutes les 2 s, c'est le défaut
      //   signalé — et je l'avais corrigé pour le bloc de carte en l'oubliant ici, un cran
      //   plus bas. Ne rien détruire ne suffit pas : il faut aussi ne rien remuer.
      if (dejaLa.zone.parentNode !== bloc) bloc.appendChild(dejaLa.zone);
      if (dejaLa.noms.join("\u0000") !== noms.join("\u0000")) {
        remplirNomsDisposition(dejaLa.sel, noms);
        dejaLa.noms = noms;
      }
      return;
    }

    const zone = document.createElement("div");
    zone.className = "dl-dispositions";
    zone.dataset.dl = "dispositions";
    zone.dataset.carte = cle;

    const sel = document.createElement("select");
    sel.className = "ctl-input";
    sel.dataset.dl = "disp-sel";
    remplirNomsDisposition(sel, noms);
    zone.appendChild(sel);

    const bRappel = document.createElement("button");
    bRappel.type = "button"; bRappel.className = "btn";
    bRappel.textContent = T("disp.rappeler", "Rappeler");
    bRappel.addEventListener("click", () => rappelerDisposition(g, sel.value));
    zone.appendChild(bRappel);

    const champ = document.createElement("input");
    champ.type = "text"; champ.className = "ctl-input";
    champ.dataset.dl = "disp-nom";
    champ.placeholder = T("disp.nom", "nom de la disposition");
    zone.appendChild(champ);

    const bSave = document.createElement("button");
    bSave.type = "button"; bSave.className = "btn";
    bSave.textContent = T("disp.enregistrer", "Enregistrer l'état actuel");
    bSave.addEventListener("click", () => enregistrerDisposition(g, champ.value));
    zone.appendChild(bSave);

    // ── IMPORT / EXPORT ────────────────────────────────────────────────────────────────
    // ★ Une disposition se transporte d'une machine à l'autre : c'est tout l'intérêt d'avoir
    //   un câblage reproductible. L'export écrit un fichier, l'import FUSIONNE — il n'efface
    //   jamais ce qui est déjà là, sinon importer une disposition en perdrait trois.
    // ★ REPLIÉ : deux gestes RARES n'ont pas à occuper la barre en permanence (demandé le
    //   20/10). Et le champ de fichier natif — « Choisir le fichier / aucun fichier
    //   sélectionné » — est masqué : il est large, il n'obéit pas à la charte, et il annonce
    //   un état dont personne n'a besoin. C'est un bouton de la charte qui le déclenche.
    const menu = document.createElement("details");
    menu.className = "dl-menu";
    menu.dataset.dl = "disp-menu";
    const resume = document.createElement("summary");
    resume.textContent = T("disp.menu", "Import / export");
    menu.appendChild(resume);
    const corps = document.createElement("div");
    corps.className = "dl-menu-corps";
    menu.appendChild(corps);

    const bExport = document.createElement("button");
    bExport.type = "button"; bExport.className = "btn";
    bExport.textContent = T("disp.exporter", "Exporter");
    bExport.addEventListener("click", () => exporterDispositions(g));
    corps.appendChild(bExport);

    const champImport = document.createElement("input");
    champImport.type = "file";
    champImport.accept = ".json,application/json";
    champImport.dataset.dl = "disp-import";
    champImport.hidden = true;
    champImport.addEventListener("change", () => importerDispositions(g, champImport));
    corps.appendChild(champImport);

    const bImport = document.createElement("button");
    bImport.type = "button"; bImport.className = "btn";
    bImport.textContent = T("disp.importer", "Importer un fichier");
    bImport.addEventListener("click", () => champImport.click());
    corps.appendChild(bImport);

    zone.appendChild(menu);

    bloc.appendChild(zone);
    zonesDisp.set(cle, { zone: zone, sel: sel, noms: noms });
  }

  function remplirNomsDisposition(sel, noms) {
    const garde = sel.value;                 // on ne perd pas la sélection en cours
    sel.textContent = "";
    const vide = document.createElement("option");
    vide.value = ""; vide.textContent = T("disp.choisir", "— disposition enregistrée —");
    sel.appendChild(vide);
    noms.forEach((n) => {
      const o = document.createElement("option");
      o.value = n; o.textContent = n;
      sel.appendChild(o);
    });
    if (noms.indexOf(garde) >= 0) sel.value = garde;
  }

  // L'état ACTUEL des connecteurs de cette carte, réduit à ce qu'une disposition retient.
  function etatDeLaCarte(g) {
    const cfg = configDeployee(etat);
    return g.map((sub) => {
      const pid = Number(sub.persistent_id);
      const ent = cfg.entrees.get(pid), sor = cfg.sorties.get(pid);
      if (ent) return { persistent_id: pid, role: "entree",
                        audio: Number(ent.audio || 0), anc: !!ent.anc };
      if (sor) {
        const e = { persistent_id: pid, role: "sortie", mode: sor.mode || "" };
        if (sor.preroll) e.preroll = Number(sor.preroll);
        // ★ L'ATTENTE DES TRANCHES EST RETENUE, comme le format et la pré-charge : une
        //   disposition qui la perdrait ferait resaccader la sortie au premier rappel, sans
        //   qu'un seul message ne l'explique.
        if (sor.slice_wait_ms) e.slice_wait_ms = Number(sor.slice_wait_ms);
        if (sor.key_persistent_id) e.key_persistent_id = Number(sor.key_persistent_id);
        return e;
      }
      return { persistent_id: pid, role: "libre" };
    });
  }

  async function enregistrerDisposition(g, nom) {
    nom = String(nom || "").trim();
    if (!nom) { message(T("disp.nom_requis", "Donnez un nom à cette disposition.")); return; }
    const cle = cleCarte(g);
    const tout = JSON.parse(JSON.stringify((config && config.layouts) || {}));
    tout[cle] = tout[cle] || {};
    tout[cle][nom] = etatDeLaCarte(g);
    // On n'envoie QUE les dispositions : le reste de la configuration n'a pas bougé, et le
    // renvoyer exposerait à l'écraser avec une copie périmée de ce qu'on affiche.
    await posterConfig({ layouts: tout },
                       T("disp.enregistree", "Disposition « {n} » enregistrée.").replace("{n}", nom));
  }

  async function rappelerDisposition(g, nom) {
    nom = String(nom || "").trim();
    if (!nom) { message(T("disp.choisir_requis", "Choisissez une disposition à rappeler.")); return; }
    const dispo = dispositionsDe(cleCarte(g))[nom];
    if (!dispo) { message(T("disp.introuvable", "Disposition introuvable.")); return; }

    // ⚠ UN RAPPEL RECONFIGURE DES CONNECTEURS, donc il peut COUPER ce qui passe. On NOMME les
    //   prises concernées avant d'agir — « des sources vont être coupées » sans dire lesquelles
    //   n'est pas un consentement que quelqu'un puisse évaluer.
    const vivantes = g.filter((sub) => sub.signal
      && (sub.signal.entree_verrouillee || sub.signal.mode_detecte))
      .map((sub) => nomPhysique(sub.persistent_id));
    const q0 = T("disp.confirmer", "Rappeler « {n} » ? Les connecteurs de cette carte sont "
                 + "réaffectés.").replace("{n}", nom)
      + (vivantes.length ? " " + T("disp.confirmer_signal",
          "Un signal est présent sur : {p}.").replace("{p}", vivantes.join(", ")) : "");
    if (!window.confirm(q0)) return;

    const cfg = configDeployee(etat);
    const miens = new Set(g.map((x) => Number(x.persistent_id)));
    const entrees = [], sorties = [];
    cfg.entrees.forEach((v, k) => { if (!miens.has(k)) entrees.push(v); });
    cfg.sorties.forEach((v, k) => { if (!miens.has(k)) sorties.push(v); });
    dispo.forEach((e) => {
      const pid = Number(e.persistent_id);
      if (e.role === "entree") {
        const avant = cfg.entrees.get(pid) || {};
        entrees.push(avecReperes({ persistent_id: pid, flow: avant.flow || "",
                                   audio: Number(e.audio || 0), anc: !!e.anc }));
      } else if (e.role === "sortie") {
        // Le flux SURVIT au rappel quand la prise reste une sortie : la disposition ne décrit
        // pas le câblage, elle ne doit donc pas l'effacer.
        const avant = cfg.sorties.get(pid) || {};
        const o = { persistent_id: pid, flow: avant.flow || "", mode: e.mode || avant.mode || "" };
        if (e.preroll) o.preroll = Number(e.preroll);
        if (e.slice_wait_ms) o.slice_wait_ms = Number(e.slice_wait_ms);
        if (e.key_persistent_id) {
          o.key_persistent_id = Number(e.key_persistent_id);
          o.key_flow = avant.key_flow || "";
        }
        // Le câblage audio SURVIT au rappel, les DEUX groupes : ne reporter que le premier
        // ferait taire les canaux 9-16 au premier rappel de disposition, sans rien dire.
        if (avant.audio_flow) {
          o.audio_flow = avant.audio_flow;
          o.audio_channels = avant.audio_channels || 8;
          if (avant.audio_flow_2) o.audio_flow_2 = avant.audio_flow_2;
        }
        sorties.push(avecReperes(o));
      }
      // « libre » : la prise n'est simplement reportée nulle part.
    });
    await posterConfig({ inputs: entrees, outputs: sorties },
                       T("disp.rappelee", "Disposition « {n} » appliquée.").replace("{n}", nom));
  }

  function exporterDispositions(g) {
    const cle = cleCarte(g);
    const dispo = dispositionsDe(cle);
    if (!Object.keys(dispo).length) {
      message(T("disp.rien_a_exporter", "Aucune disposition enregistrée sur cette carte."));
      return;
    }
    // Le fichier porte le MODÈLE de la carte et la date : à la relecture, on saura d'où il
    // vient. Un export anonyme se retrouve six mois plus tard sans qu'on sache quoi en faire.
    const doc = { format: "decklink_io.dispositions", version: 1,
                  carte: (g[0] && g[0].model) || "", exporte_le: new Date().toISOString(),
                  dispositions: dispo };
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = ((g[0] && g[0].model) || "decklink").replace(/[^\w.-]+/g, "_")
                 + "-dispositions.json";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    message(T("disp.exportee", "Dispositions exportées."));
  }

  async function importerDispositions(g, champ) {
    const f = champ && champ.files && champ.files[0];
    if (!f) return;
    let doc = null;
    try {
      doc = JSON.parse(await f.text());
    } catch (e) {
      message(T("disp.import_illisible", "Fichier illisible : ce n'est pas un export de dispositions."));
      return;
    }
    // ⚠ ON VÉRIFIE LA FORME AVANT D'ÉCRIRE. Un fichier quelconque accepté tel quel mettrait
    //   n'importe quoi dans la configuration du conteneur, et le rappel réaffecterait des
    //   connecteurs d'après des données qui n'en décrivent pas.
    const recues = doc && doc.dispositions;
    if (!recues || typeof recues !== "object" || doc.format !== "decklink_io.dispositions") {
      message(T("disp.import_inconnu", "Ce fichier n'est pas un export de dispositions DeckLink."));
      return;
    }
    const valides = {};
    Object.keys(recues).forEach((nom) => {
      const d = recues[nom];
      if (Array.isArray(d) && d.every((e) => e && e.persistent_id != null
                                      && ["entree", "sortie", "libre"].indexOf(e.role) >= 0)) {
        valides[nom] = d;
      }
    });
    const n = Object.keys(valides).length;
    if (!n) {
      message(T("disp.import_vide", "Aucune disposition exploitable dans ce fichier."));
      return;
    }
    const cle = cleCarte(g);
    const tout = JSON.parse(JSON.stringify((config && config.layouts) || {}));
    // FUSION, jamais remplacement : importer une disposition ne doit pas en effacer trois.
    tout[cle] = Object.assign({}, tout[cle] || {}, valides);
    champ.value = "";
    await posterConfig({ layouts: tout },
      T("disp.importees", "{n} disposition(s) importée(s).").replace("{n}", String(n)));
  }

  async function posterConfig(partiel, msgOk) {
    if (envoiEnCours) return;
    envoiEnCours = true;
    message(T("note.envoi", "Enregistrement et redéploiement…"));
    try {
      const rep = await fetch(cfgApi("/plugin_config"), {
        method: "POST", credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ params: partiel, confirm: true }),
      });
      const d = await rep.json().catch(() => ({}));
      if (!rep.ok) throw new Error((d && d.error) || ("HTTP " + rep.status));
      message(msgOk);
      // Un rappel réaffecte les prises : même attente qu'un réglage. Les dispositions seules
      // s'écrivent sans redéployer, il n'y a rien à attendre.
      if (partiel.inputs || partiel.outputs) { retenirEnvoi(partiel); afficherAttente(texteAttente()); }
      config = avecEnvoiEnCours(await lireConfig()) || config;
      dessiner();
    } catch (e) {
      message(T("disp.echec", "Échec : ") + ((e && e.message) ? e.message : String(e)));
    } finally {
      envoiEnCours = false;
    }
  }

  //: Blocs de carte déjà construits, par carte. ★★ ON NE VIDE PLUS LE BANDEAU (20/10).
  //: La version précédente faisait `hote.textContent = ""` à chaque redessin — toutes les 2 s —
  //: et tout ce qui vivait dedans était détruit, ou devait être déménagé avant puis replacé
  //: après. Trois défauts sont nés de là, signalés l'un après l'autre par l'exploitant :
  //:   · le repli « Installation » se refermait sous les doigts ;
  //:   · le menu de réglage disparaissait complètement de la page ;
  //:   · les listes déroulantes se repliaient toutes les deux secondes — car DÉPLACER un
  //:     élément suffit à refermer une liste ouverte, même sans le détruire.
  //: La cause était commune, et elle est traitée à la racine : les blocs sont RÉUTILISÉS, et
  //: seul le ruban des pastilles — qui ne porte aucun état de saisie — est reconstruit.
  const blocsCarte = new Map();

  function dessinerBandeau(subs, cfg, st) {
    const hote = EL.querySelector('[data-dl="bandeau"]');
    const groupes = grouperParCarte(subs);
    const vus = new Set();

    groupes.forEach((g) => {
      const cle = String((g[0] && g[0].persistent_id) || "");
      vus.add(cle);
      let ref = blocsCarte.get(cle);
      if (!ref) {
        const bloc = document.createElement("section");
        bloc.className = "dl-carte-bloc";
        bloc.dataset.carte = cle;
        const h = document.createElement("h5");
        h.className = "dl-carte-titre";
        bloc.appendChild(h);
        const ruban = document.createElement("div");
        ruban.className = "dl-bandeau-carte";
        bloc.appendChild(ruban);
        ref = { bloc: bloc, titre: h, ruban: ruban };
        blocsCarte.set(cle, ref);
      }
      ref.titre.hidden = groupes.length < 2;
      ref.titre.textContent = (g[0] && g[0].model) || T("carte.inconnue", "carte DeckLink");
      // ⚠ On ne rattache le bloc QUE s'il n'est pas déjà là : `appendChild` sur un élément déjà
      //   présent le détache et le rattache, ce qui referme toute liste ouverte à l'intérieur.
      if (ref.bloc.parentNode !== hote) hote.appendChild(ref.bloc);
      dessinerDispositions(ref.bloc, g);
      ref.ruban.textContent = "";
      dessinerCarte(ref.ruban, g, cfg, st);
    });

    // Une carte retirée de la machine emporte son bloc, sinon il resterait affiché à vide.
    Array.from(blocsCarte.keys()).forEach((cle) => {
      if (!vus.has(cle)) {
        const ref = blocsCarte.get(cle);
        if (ref && ref.bloc && ref.bloc.parentNode) ref.bloc.parentNode.removeChild(ref.bloc);
        blocsCarte.delete(cle);
      }
    });

    placerEditeurSousSaCarte();
  }

  // Place l'éditeur dans le bloc de la carte réglée — UNE SEULE FOIS, quand il n'y est pas
  // déjà. Le déplacer à chaque redessin refermait les listes déroulantes qu'il contient.
  function placerEditeurSousSaCarte() {
    const edit = q("editeur");
    if (!edit || choisi == null) return;
    const g = grouperParCarte(sousPeripheriques(inventaire))
      .find((x) => x.some((y) => Number(y.persistent_id) === Number(choisi)));
    if (!g) return;
    const ref = blocsCarte.get(String(g[0] && g[0].persistent_id));
    if (ref && ref.bloc && edit.parentNode !== ref.bloc) ref.bloc.appendChild(edit);
  }

  function dessinerCarte(hote, subs, cfg, st) {
    // Les mesures vivantes, indexées par identité STABLE. `voies` porte l'état réel des sorties
    // (une sortie Fill & Key a deux connecteurs) ; `outputs` en est la vue dérivée.
    const parPid = new Map();
    ((st && st.inputs) || []).concat((st && st.voies) || [], (st && st.outputs) || [])
      .forEach((x) => { if (x && x.persistent_id != null && !parPid.has(Number(x.persistent_id)))
                          parPid.set(Number(x.persistent_id), x); });
    // La référence n'est pas un sous-périphérique : elle n'apparaît dans aucune énumération.
    // On la dessine à part, parce qu'elle EXISTE sur l'équerre et que l'exploitant la cherche.
    //
    // ★★ ORDRE ÉTABLI ET VÉRIFIÉ LE 2026-10-19. Ce fut la dernière inconnue du chantier : nos
  //    numéros sont ceux des SOUS-PÉRIPHÉRIQUES, et rien dans le SDK ne dit à quel BNC de
  //    l'équerre chacun correspond. Deux preuves indépendantes la closent :
  //      1. le schéma constructeur place « Reference In » à l'extrémité CÔTÉ PCI Express ;
  //      2. l'exploitant a débranché le BNC qui SUIT la référence, et c'est notre prise 1 qui
  //         s'est éteinte — relevé sur la carte pendant le débranchement.
  //    Donc, en partant de la carte mère : REF (0), puis 1, 2, 3, 4. C'est l'ordre du bandeau,
  //    et la numérotation 0 pour la référence est la convention de l'exploitant.
  //
  // ★ ELLE EST À GAUCHE, et c'est le testeur qui l'a établi (2026-10-02). Raisonnement par
    //   élimination : son câble occupe le DEUXIÈME connecteur physique en partant de la gauche,
    //   et la carte le voit sur son PREMIER sous-périphérique. Si le premier connecteur était
    //   SDI 1, son signal apparaîtrait en 2 ; il apparaît en 1, donc le premier connecteur n'est
    //   pas une entrée SDI — c'est la REF. Un second témoin, un mois plus tôt, avait dit la même
    //   chose en branchant son générateur de synchro sur ce connecteur-là.
    //   Je la dessinais à DROITE, par simple commodité de code. Un dessin faux est pire qu'une
    //   absence de dessin : il envoie se brancher au mauvais endroit avec confiance.
    // ⚠ N'INVENTONS PAS UN CONNECTEUR. On dessinait une REF sur TOUTE carte : vrai sur la Duo 2,
    //   mais rien dans le SDK ne dit qu'un modèle en possède une. Sur une carte sans entrée de
    //   référence, on aurait dessiné une prise fantôme — la faute même qu'on s'interdit depuis
    //   que la REF était du mauvais côté. La preuve dont on dispose : la carte RAPPORTE ou non un
    //   état de référence (`null` = l'attribut n'existe pas). C'est une preuve d'EXISTENCE, pas
    //   de position : la position vient du témoignage de deux exploitants et ne vaut que pour la
    //   Duo 2, d'où l'avertissement sous le bandeau.
    // ── REPÈRE « CÔTÉ PCI » ───────────────────────────────────────────────────────────────
    // ★ L'ORIENTATION EST ÉTABLIE PAR LE SCHÉMA DU CONSTRUCTEUR (19/10, apporté par
    //   l'utilisateur) : sur l'équerre d'une Duo 2, les quatre BNC SDI sont alignés et
    //   « Reference In » est le connecteur d'EXTRÉMITÉ, du côté du connecteur PCI Express.
    //   Sans ce repère, un dessin horizontal ne dit pas par quel bout le lire, et l'exploitant
    //   peut le retourner mentalement — on l'envoie alors brancher au mauvais endroit avec
    //   confiance, ce qu'on s'interdit depuis que la REF était du mauvais côté.
    // ⚠ CE QUE LE SCHÉMA NE DIT PAS : lequel des quatre SDI porte le sous-périphérique 0. La
    //   correspondance entre l'ordre de la carte et l'ordre physique des SDI entre eux reste
    //   NON VÉRIFIÉE — l'avertissement sous le bandeau le dit toujours.
    const repere = document.createElement("div");
    repere.className = "dl-cote-pci";
    repere.textContent = T("bandeau.cote_pci", "côté PCI");
    repere.title = T("bandeau.cote_pci_aide",
      // ★ LE SEUL REPÈRE QUI NE DÉPEND PAS DU MONTAGE (rappelé par l'exploitant le 20/10).
      //   J'avais reformulé en « vus de l'arrière du serveur, de haut en bas » pour éviter le
      //   mot « équerre » : c'était plus clair ET FAUX. Rack avec riser, tour, châssis
      //   retourné — « en haut » change avec l'installation. Le connecteur PCI Express, lui,
      //   est une propriété de la CARTE : il est toujours au même bout.
      "Ce bout du dessin est celui du connecteur PCI Express — la languette dorée qui " +
      "s'enfiche dans la carte mère. C'est le seul repère qui ne dépende pas de la façon dont " +
      "le serveur est monté.");
    hote.appendChild(repere);

    const refRapportee = subs.some((s) => s.signal
        && s.signal.reference_verrouillee !== null && s.signal.reference_verrouillee !== undefined);
    const refConnue = subs.some((s) => s.signal && s.signal.reference_verrouillee);
    // ⚠ CE BLOC NE DOIT PAS POUVOIR INTERROMPRE LE DESSIN DES PRISES. Il y avait ici un
    //   `return` quand la carte ne rapportait pas de référence : sur un modèle sans entrée de
    //   REF, le bandeau entier restait VIDE — plus une seule prise. Le relevé Duo 2 en rapporte
    //   une, donc ni la console ni le banc ne l'ont vu. Absence de REF = on saute la REF, rien
    //   d'autre.
    if (refRapportee) {
      const ref = document.createElement("div");
      ref.className = "dl-prise";
      ref.dataset.role = "ref";
      ref.dataset.signal = refConnue ? "1" : "0";
      ref.title = T("ref.titre", "Entrée de référence (genlock)");
      const bncR = document.createElement("div"); bncR.className = "dl-bnc";
      // ★ LA RÉFÉRENCE PORTE LE NUMÉRO 0 — convention proposée par l'exploitant le 19/10 et
      //   VÉRIFIÉE sur la carte le même jour : en partant du côté carte mère, la référence est
      //   le connecteur d'extrémité (schéma constructeur), et le BNC suivant est le n° 1 — son
      //   débranchement a bien éteint notre prise 1. L'ordre du bandeau correspond donc à
      //   l'équerre, ce qui n'était jusque-là qu'une hypothèse.
      const numR = document.createElement("span");
      numR.className = "dl-bnc-num";
      numR.textContent = "0";
      bncR.appendChild(numR);
      const etiqR = document.createElement("div"); etiqR.className = "dl-etiq";
      etiqR.textContent = T("ref.etiquette", "REF");
      const rleR = document.createElement("div"); rleR.className = "dl-role";
      // ⚠ ON N'AFFICHE PLUS « VERROUILLÉE » (19/10). La carte ne rapporte ce verrou que sur UN
      //   sous-périphérique, toujours le même, quoi qu'on branche et quoi qu'on fasse émettre —
      //   deux hypothèses éprouvées sur la carte, deux réfutées (l'entrée qui prendrait le pas
      //   sur la référence ; la sortie active qui conditionnerait le verrou). Un voyant qu'on
      //   ne sait pas expliquer finit par être cru. On s'en tient à ce qui est stable et
      //   cohérent sur les quatre prises : la référence est PRÉSENTE, et voici son format. Le
      //   genlock d'une sortie est celui que notre binaire MESURE sur la cadence émise.
      rleR.textContent = (refConnue || formatReference(subs))
        ? T("ref.presente", "présente") : T("ref.absente", "absente");
      ref.appendChild(bncR); ref.appendChild(etiqR); ref.appendChild(rleR);
      // La REF ne se règle pas : elle n'a ni rôle ni flux. Lui donner un menu ouvrirait un
      // panneau vide — exactement ce qui fait passer un clic pour mort. On montre son FORMAT,
      // qui est la seule chose qu'on puisse en dire d'utile (relevé le 19/10 : noir PAL).
      const fmtR = document.createElement("div");
      fmtR.className = "dl-mesure-prise";
      fmtR.textContent = formatReference(subs);
      ref.appendChild(fmtR);
      hote.appendChild(ref);      // PREMIER élément : la REF est à gauche
    }

    // ── PAIRES FILL & KEY ─────────────────────────────────────────────────────────────────
    // Une sortie Fill & Key LIE deux connecteurs : le remplissage et la découpe partent
    // ensemble, partagent format et pré-charge, et n'ont aucun sens l'un sans l'autre. Rien ne
    // le montrait — les deux prises se lisaient comme deux sorties indépendantes. On leur donne
    // un numéro de paire commun : c'est ce numéro, et non la proximité, qui fait le lien, car
    // les deux connecteurs ne sont pas forcément voisins sur l'équerre.
    const paire = new Map();
    let nPaire = 0;
    cfg.sorties.forEach((o, pidFill) => {
      if (!o || !o.key_persistent_id) return;
      nPaire += 1;
      paire.set(Number(pidFill), nPaire);
      paire.set(Number(o.key_persistent_id), nPaire);
    });

    subs.forEach((s, i) => {
      const r = role(s, cfg);
      const nFK = paire.get(Number(s.persistent_id)) || 0;
      const prise = document.createElement("div");
      prise.className = "dl-prise";
      prise.dataset.role = r;
      const sig = (s.signal && s.signal.entree_verrouillee) ? "1" : "0";
      prise.dataset.signal = sig;
      // L'identité STABLE portée par le DOM : c'est elle qui sert au clic, aux contrôles et au
      // banc. Retrouver la prise par son rang dans la liste marcherait jusqu'au premier
      // changement de profil, qui renumérote.
      prise.dataset.pid = String(s.persistent_id || "");
      if (nFK) prise.dataset.fk = String(nFK);
      prise.title = (s.display_name || "") + " — " + (s.persistent_id || "") +
        (r === "inactif" ? " — " + T("titre.inactive", "éteinte par le profil de la carte") : "");

      const bnc = document.createElement("div");
      bnc.className = "dl-bnc";
      const num = document.createElement("span");
      num.className = "dl-bnc-num";
      num.textContent = String(i + 1);
      bnc.appendChild(num);

      const etiq = document.createElement("div");
      etiq.className = "dl-etiq";
      // ★ L'ÉTIQUETTE PORTE LE NOM DONNÉ PAR LA CARTE — demandé par l'exploitant le 19/10.
      //   Je l'avais remplacé par la position (« SDI 2 ») pendant la parenthèse où le bandeau
      //   était trié par position physique : le nom du SDK embarque son index d'ÉNUMÉRATION, et
      //   il contredisait alors la pastille. Le tri ayant été retiré, les deux coïncident de
      //   nouveau — et c'est ce nom que l'exploitant retrouve dans les outils Blackmagic.
      etiq.textContent = s.display_name || (T("prise.etiquette", "SDI") + " " + (i + 1));

      const rle = document.createElement("div");
      rle.className = "dl-role";
      rle.textContent = (LIBELLE_ROLE[r] || LIBELLE_ROLE.libre)();
      if (nFK) {
        const fk = document.createElement("span");
        fk.className = "dl-fk";
        // Le numéro dit QUELLE paire, ce qui compte dès qu'il y en a deux : « F&K » seul
        // laisserait croire que les quatre prises marquées forment un seul groupe.
        fk.textContent = " " + T("fk.etiquette", "F&K") + " " + nFK;
        fk.title = T("fk.aide",
          "Ces deux connecteurs forment une seule sortie Fill & Key : ils partent ensemble et " +
          "partagent format et pré-charge.");
        rle.appendChild(fk);
      }

      const mes = document.createElement("div");
      mes.className = "dl-mesure-prise";
      // Le mode d'une sortie vit dans la configuration (`outputs[].mode`), pas dans l'état de
      // la carte. Pour une voie de DÉCOUPE, c'est le mode de la sortie qui la porte : les deux
      // connecteurs d'un Fill & Key partagent le format par construction.
      const _o = cfg.sorties.get(Number(s.persistent_id))
              || cfg.decoupes.get(Number(s.persistent_id));
      mes.textContent = mesurePrise(s, parPid.get(Number(s.persistent_id)) || null, r,
                                    _o && _o.mode);

      prise.appendChild(bnc); prise.appendChild(etiq); prise.appendChild(rle);
      prise.appendChild(mes);
      if (editable() && r !== "inactif") {
        prise.addEventListener("click", () => ouvrirEditeur(s));
      } else if (r === "inactif") {
        // Cliquable en apparence puis inerte serait pire que muet : on dit POURQUOI.
        prise.addEventListener("click", () => message(T("note.inactive",
          "Cette prise est éteinte par le profil actif de la carte. Rien ne peut y passer tant " +
          "que le profil n'a pas changé — et aucun câble n'y changera quoi que ce soit.")));
      }
      prise.dataset.choisie = (choisi != null && Number(choisi) === Number(s.persistent_id)) ? "1" : "0";
      hote.appendChild(prise);
    });

  }

  // Le TABLEAU « Prises » a été retiré le 19/10 : il répétait le bandeau ligne pour ligne
  // (même nom, même rôle), et la seule chose qu'il ajoutait — format et cadence — est
  // désormais portée par la prise elle-même. Deux rendus d'une même vérité finissent par se
  // contredire : c'est exactement ce qui venait d'arriver à l'état des sorties.

  // ── PROFIL DE LA CARTE ─────────────────────────────────────────────────────────────────
  // ★ ON MONTRE CE QU'ON SAIT, ET ON DIT CE QU'ON NE SAIT PAS (décidé le 16/10). Le SDK ne
  //   décrit PAS la disposition des connecteurs d'un profil : on ne la découvre qu'après l'avoir
  //   appliqué. On affiche donc la STRUCTURE (nombre de sous-périphériques, duplex) et le rayon
  //   annoncé — en disant qu'il sous-estime, parce qu'il l'a fait : le 2026-09-21 il annonçait
  //   un sous-périphérique et la bascule en a éteint deux.
  //
  //   Dessiner une disposition déduite du modèle de carte réglerait l'affichage et violerait la
  //   règle du chantier — aucune branche par modèle. Et un schéma inventé est pire qu'un schéma
  //   absent : on s'y fie.
  const DUPLEX_MOT = { fd: () => T("profil.full", "entrée ET sortie en même temps"),
                       hd: () => T("profil.half", "entrée OU sortie, au choix") };

  function decrireProfil(id) {
    // Le four-CC porte sa propre structure : « 2dhd » = 2 sous-périphériques, half-duplex.
    const m = /^(\d)d(fd|hd)$/.exec(String(id || ""));
    if (!m) return null;
    return { n: Number(m[1]), duplex: m[2] };
  }



  // ⚠ LE BLOC DE PROFIL DU PANNEAU N'EXISTE PLUS (20/10) : il était commun aux deux cartes et
  //   visait toujours la première énumérée. On garde la fonction pour MASQUER l'ancien bloc :
  //   le gabarit le porte encore, et un bloc orphelin resté visible proposerait de reconfigurer
  //   « la » carte sans dire laquelle — exactement ce qu'on vient de retirer.
  // ★★ CE COMMENTAIRE A MENTI PENDANT SEPT JOURS, ET C'EST LUI QU'ON EST VENU LIRE (27/10).
  //    Il renvoyait à `dessinerInstallationCarte` — « chaque carte porte le sien, sous son
  //    propre ruban » — une fonction que `a214a67` (0.29.12, 20/10) a supprimée en même temps
  //    que le bloc commun. Elle n'existe nulle part. Une session cherchant pourquoi le testeur ne
  //    pouvait pas basculer de profil depuis la page a conclu à une régression et s'apprêtait à
  //    la rétablir ; le retrait était en réalité DÉCIDÉ (on ne voyait pas l'intérêt d'une
  //    bascule à la main). Un renvoi vers du code disparu coûte plus cher qu'une absence : il
  //    envoie quelqu'un reconstruire ce qu'on a retiré exprès.
  // ⚠ LA BASCULE DE PROFIL N'EST DONC PLUS EXPOSÉE DANS L'INTERFACE. `appliquerProfil` et
  //   `posterProfil`, plus bas, n'ont AUCUN appelant depuis 0.29.12. Elles sont conservées, pas
  //   oubliées : elles portent le consentement NOMMÉ de la décision du 19/10 (dire QUELLES
  //   prises on va couper avant de couper), et la normalisation automatique au démarrage —
  //   qui doit les remplacer — attend encore un arbitrage sur le refus « signal vivant ».
  //   Si cet arbitrage retient le forçage, elles n'ont plus de raison d'être et se retirent ;
  //   s'il le refuse, c'est elles qu'il faudra rebrancher. À ne pas supprimer entre-temps.
  function dessinerProfils(subs) {
    const bloc = q("bloc-profil");
    if (bloc) bloc.hidden = true;
  }


  // Le nom que l'exploitant lit sur le bandeau, pas un identifiant à 10 chiffres : c'est à lui
  // qu'on demande de consentir, donc il doit reconnaître la prise qu'il s'apprête à couper.
  function nomDePrise(pid) {
    return nomPhysique(pid);
  }

  // Le conteneur renvoie un CODE en anglais ; c'est l'interface qui le met en mots, et dans la
  // langue de l'exploitant. Une phrase remontée telle quelle par le conteneur ne se traduit pas.
  function texteDeCode(code) {
    if (!code) return "";
    const connus = {
      live_input_signal: ["profil.code.signal_vivant",
        "une source est présente sur une des prises concernées"],
      switch_in_progress: ["profil.code.deja_en_cours",
        "une bascule de profil est déjà en cours"],
      switch_failed: ["profil.code.echec", "la carte a refusé la bascule"],
    };
    const e = connus[code];
    // Code inconnu : on l'affiche BRUT plutôt que de le taire. Un code non traduit reste une
    // piste ; un message vide n'en est pas une.
    return e ? T(e[0], e[1]) : code;
  }

  // SANS APPELANT depuis 0.29.12 — cf. le pavé de `dessinerProfils`. Ne pas supprimer
  // sans avoir tranché la normalisation automatique au démarrage.
  async function posterProfil(p, cible, consentement) {
    const r = await fetch(api("/profile"), {
      method: "POST", credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ profile: p.id, persistent_id: Number(cible.persistent_id),
                             consentement: !!consentement }),
    });
    return { http: r.status, d: (await r.json().catch(() => ({}))) || {} };
  }

  // SANS APPELANT depuis 0.29.12 — cf. le pavé de `dessinerProfils`. Porte le consentement
  // NOMMÉ du 19/10 ; c'est ce qu'on perdrait en la jetant.
  async function appliquerProfil(p, subs) {
    const vivantes = subs.filter((s) => s.signal && s.signal.entree_verrouillee).length;
    const avert = T("profil.confirmer",
      "Appliquer « {p} » ? La carte est reconfigurée, les captures s'arrêtent, et des prises " +
      "peuvent devenir inutilisables.").replace("{p}", p.label || p.id) +
      (vivantes ? " " + T("profil.confirmer_signal",
        "Attention : {n} prise(s) reçoivent actuellement un signal.").replace("{n}", vivantes) : "");
    if (!window.confirm(avert)) return;
    const cible = subs[0];
    message(T("profil.encours", "Changement de profil en cours — la carte se reconfigure…"));
    try {
      let { http, d } = await posterProfil(p, cible, false);
      // ★ LE REFUS « SOURCE VIVANTE » EST UNE QUESTION, PAS UN ÉCHEC (2026-10-19). L'outil
      //   refuse dès qu'une entrée porte un signal, même si plus personne ne la capture : la
      //   console affichait alors « HTTP 200 » faute de champ d'erreur, et la bascule était
      //   impossible dès qu'un câble était branché. On demande maintenant le consentement, en
      //   NOMMANT les prises qu'on va couper — « une source va être coupée » sans dire laquelle
      //   est un consentement que personne ne peut évaluer.
      if (!d.ok && d.confirmation_requise) {
        const noms = (d.prises_vivantes || []).map(nomDePrise).join(", ");
        const q = T("profil.couper_source",
          "Une source est présente sur : {p}. Appliquer ce profil va la couper. Continuer ?")
          .replace("{p}", noms || T("profil.prise_inconnue", "une prise non identifiée"));
        if (!window.confirm(q)) {
          message(T("profil.abandon", "Changement abandonné — aucune source n'a été coupée."));
          return;
        }
        ({ http, d } = await posterProfil(p, cible, true));
      }
      if (!d.ok) throw new Error(texteDeCode(d.code) || ("HTTP " + http));
      // On annonce la disposition RÉELLE, relevée après coup — jamais celle qu'on avait prévue.
      const ap = (d.apres && Object.values(d.apres).find((v) => Array.isArray(v))) || [];
      const eteintes = ap.filter((s) => (s.duplex || "") === "inactive").length;
      message(T("profil.fait", "Profil appliqué. Disposition réelle : {n} prise(s), dont {e} éteinte(s).")
                .replace("{n}", ap.length).replace("{e}", eteintes));
      rafraichir();
    } catch (e) {
      message(T("profil.echec", "Changement de profil refusé : ") + (e && e.message ? e.message : e));
    }
  }

  function dessiner() {
    const subs = sousPeripheriques(inventaire);
    const cfg = configDeployee(etat);
    // ★ LE TITRE DU PANNEAU NE PEUT PAS ÊTRE CELUI D'UNE SEULE CARTE (20/10). Il affichait le
    //   modèle du PREMIER sous-périphérique énuméré : avec une Quad 2 et une Duo 2 dans la même
    //   machine, tout le panneau s'intitulait « DeckLink Quad 2 », et les douze prises
    //   semblaient appartenir à cette carte-là. Signalé en recette, à juste titre.
    //   Avec plusieurs cartes, on compte ; avec une seule, on la nomme.
    const groupes0 = grouperParCarte(subs);
    const elModele = EL.querySelector('[data-dl="modele"]');
    if (groupes0.length > 1) {
      elModele.textContent = T("cartes.nombre", "{n} cartes DeckLink")
        .replace("{n}", String(groupes0.length));
    } else {
      const s0 = subs[0] || {};
      elModele.textContent = s0.model || T("carte.inconnue", "carte DeckLink");
    }
    if (versionPilote) {
      elModele.textContent += " · " + T("pilote.version", "Desktop Video {v}")
        .replace("{v}", versionPilote);
    }
    // ⚠ PLUS DE PROFIL DANS L'EN-TÊTE : il valait pour la première carte seulement, et le
    //   profil n'est de toute façon plus un réglage exposé (décision du 20/10).
    EL.querySelector('[data-dl="profil"]').textContent = "";

    dessinerProfils(subs);
    dessinerBandeau(subs, cfg, etat);

    const avert = EL.querySelector('[data-dl="avert"]');
    const inactives = subs.filter((s) => (s.duplex || "") === "inactive").length;
    // ★★ L'AVERTISSEMENT SUR L'ORDRE EST RETIRÉ (20/10, sur confirmation de l'exploitant).
    //    Il a vécu tant que l'ordre n'était qu'une concordance de sources : le schéma du
    //    constructeur et le `topological_id` disaient la même chose, mais personne ne l'avait
    //    confronté au câble. C'est fait, sur les DEUX cartes, par l'exploitant lui-même.
    // ⚠ CE QU'IL A FALLU POUR EN ARRIVER LÀ, et qui explique qu'on ne l'ait pas retiré plus
    //    tôt : je l'avais supprimé le 19 au matin sur une vérification qui ne POUVAIT PAS
    //    trancher — deux débranchements aux extrémités, qui laissent une permutation du milieu
    //    indétectable. Il a fallu le remettre. Un avertissement ne se retire pas parce qu'on
    //    croit savoir, mais parce que quelqu'un a vérifié.
    const bouts = [];
    if (inactives) {
      bouts.push(T("avert.inactives",
        "Des prises sont ÉTEINTES par le profil actif : un câble branché dessus ne donne rien, " +
        "et rien sur le châssis ne le dit."));
    }
    cartesEnSurcharge(Array.from(cfg.sorties.values())).forEach((x) => {
      bouts.push(T("avert.trop_de_sorties",
        "{c} porte {n} sorties, et n'en tient que {max} : au-delà, des sorties émettent en "
        + "retard permanent et LEUR SON DISPARAÎT, sans que les compteurs le montrent. Ce n'est "
        + "pas encore fonctionnel — ramenez-en à {max} sur cette carte.")
        .replace(/\{c\}/g, x.carte || T("avert.cette_carte", "cette carte"))
        .replace(/\{n\}/g, String(x.n))
        .replace(/\{max\}/g, String(PLAFOND_SORTIES_PAR_CARTE)));
    });
    avert.textContent = bouts.join(" ");
    avert.hidden = !bouts.length;
  }

  const q = (nom) => EL.querySelector('[data-dl="' + nom + '"]');

  function note(txt) {
    const n = q("note");
    if (n) { n.textContent = txt; }
  }

  // ⚠ DISTINCT DE `note` : ce message-ci s'affiche SOUS LE BANDEAU, hors de l'éditeur. Le refus
  //   de clic sur une prise éteinte y était écrit par `note`, donc dans un bloc masqué tant qu'on
  //   n'éditait rien : le clic passait pour mort, et la prise pour « pas cliquable ». Expliquer
  //   un refus dans un endroit invisible revient à ne pas l'expliquer.
  function message(txt) {
    const m = q("message");
    if (!m) return;
    m.textContent = txt || "";
    m.hidden = !txt;
  }

  function roleCourant(pid) {
    const cfg = configDeployee(etat);
    if (cfg.entrees.has(Number(pid))) return "entree";
    if (cfg.sorties.has(Number(pid))) return "sortie";
    return "libre";
  }

  // Place la FLÈCHE du menu sous la prise cliquée. Le panneau, lui, reste dans le flux normal
  // et pleine largeur : le bandeau défile horizontalement (`overflow-x: auto`), donc un panneau
  // positionné DEDANS serait rogné — invisible à 4 prises, certain à 8 sur une Quad 2. On
  // déplace donc seulement le repère, ce qui suffit à rattacher le menu à son connecteur.
  function ancrerSousLaPrise(ed, pid) {
    const bandeau = EL.querySelector('[data-dl="bandeau"]');
    // ⚠ LA PRISE N'EST PLUS UN ENFANT DIRECT DU BANDEAU depuis qu'il y a un ruban par carte :
    //   la recherche à un seul niveau ne trouvait plus rien et la flèche ne pointait nulle part.
    const trouver = (n) => {
      const kids = n && n.children ? Array.prototype.slice.call(n.children) : [];
      for (const c of kids) {
        if (c.dataset && String(c.dataset.pid) === String(pid)) return c;
        const t = trouver(c);
        if (t) return t;
      }
      return null;
    };
    const prise = trouver(bandeau);
    // Sans mise en page (banc, rendu serveur), on ne place rien et le menu reste utilisable.
    if (!prise || typeof prise.offsetLeft !== "number" || !ed.style) return;
    const x = prise.offsetLeft + (prise.offsetWidth || 0) / 2 - (bandeau.scrollLeft || 0);
    ed.style.setProperty("--dl-fleche", Math.max(18, x) + "px");
  }

  function ouvrirEditeur(sub) {
    choisi = Number(sub.persistent_id);
    aConfirmer = false;          // un consentement vaut pour CE geste, pas pour le suivant
    const cfg = configDeployee(etat);
    const ent = cfg.entrees.get(choisi) || null;
    const sor = cfg.sorties.get(choisi) || null;
    const r = roleCourant(choisi);

    // ★ PLUS DE TITRE QUI RÉPÈTE LE NOM (19/10). La prise est juste au-dessus, entourée, et son
    //   nom y est écrit : le redire en tête du panneau en faisait un « bloc » de plus au lieu
    //   d'un menu attaché à ce qu'on vient de cliquer. On garde le texte pour les lecteurs
    //   d'écran, qui n'ont pas l'entourage visuel pour faire le lien.
    const titre = q("edit-titre");
    titre.className = "dl-pour-lecteur";
    titre.textContent = nomPhysique(sub.persistent_id) + " — " +
                        T("editeur.titre", "que fait cette prise ?");
    q("role").value = r;
    q("flux-fixe").textContent = fluxProduit(choisi);
    chargerFluxDisponibles().then(() => {
      remplirFluxSortie(sor);
      remplirDecoupe(sub, sor);
    });
    q("audio").value = String((ent && ent.audio) != null ? ent.audio : 16);
    {   // 16 canaux en entrée publient DEUX flux de 8 : le dire là où le choix se fait.
        const aideE = T("editeur.audio_entree_aide", "");
        if (aideE) { q("audio").title = aideE; const l = q("lbl-audio"); if (l) l.title = aideE; }
    }
    remplirAudioSortie(sor);
    q("anc").checked = ent ? !!ent.anc : true;
    // Le format n'est PAS énuméré par la sonde : on ne peut pas proposer une liste fermée sans
    // l'inventer. On pré-remplit avec ce que la carte DÉTECTE quand elle détecte quelque chose.
    remplirModes(sub, sor);
    remplirPrecharge((sor && sor.preroll) || 0);
    remplirAttenteTranches((sor && sor.slice_wait_ms) || 0);
    remplirDecoupe(sub, sor);
    // ★ L'OUVERTURE PASSE AVANT TOUT LE RESTE, ET RIEN NE PEUT LA BLOQUER (20/10). L'éditeur a
    //   été rendu inatteignable deux fois dans la journée par des étapes qui ne servent qu'à
    //   le PLACER : une fois en le parentant à un bloc régénéré, une fois par un appel posé
    //   avant lui. L'ordre correct est : on ouvre, PUIS on embellit — et l'embellissement est
    //   protégé, parce qu'une flèche mal posée ne vaut pas un menu qui ne s'ouvre pas.
    const ed = q("editeur");
    if (!ed) {
      // Si l'élément a disparu de la page, le dire au lieu de laisser un clic sans effet.
      message(T("editeur.absent",
                "Le panneau de réglage est introuvable dans la page — rechargez-la (Ctrl-Maj-R)."));
      return;
    }
    ed.hidden = false;
    // Placement et ancrage sont COSMÉTIQUES : protégés, ils ne peuvent pas empêcher l'ouverture.
    try { placerEditeurSousSaCarte(); } catch (e) { /* jamais bloquant */ }
    try { ancrerSousLaPrise(ed, choisi); } catch (e) { /* jamais bloquant */ }
    try { majChamps(); } catch (e) {
      message(T("editeur.partiel", "Le panneau s'ouvre, mais un champ n'a pas pu être rempli."));
    }
    note("");
    message("");
    dessiner();
  }

  // Le nom d'un flux que NOUS produisons est une identité, pas une préférence : il suit la
  // convention du parc « <hôte>_<rang> », rang comptant à partir de 1 parmi les entrées. Le
  // calculer ici plutôt que de le faire saisir supprime la faute de frappe — qui, elle, ne se
  // voit qu'après coup, chez tous ceux qui référencent ce nom.
  function fluxProduit(pid) {
    const h = (etat && etat.hostname) || "decklink";
    const cfg = configDeployee(etat);
    const ent = cfg.entrees.get(Number(pid));
    if (ent && ent.flow) return ent.flow;          // déjà nommé : on ne renomme JAMAIS tout seul
    const pris = new Set();
    cfg.entrees.forEach((v) => { if (v && v.flow) pris.add(v.flow); });
    let n = 1;
    while (pris.has(h + "_" + n)) n++;
    return h + "_" + n;
  }

  // ★ LA LISTE DES FLUX EST UNE DONNÉE, PLUS LE CONTENU D'UN CHAMP (19/10). Le flux à ÉMETTRE
  //   ne se règle plus ici — c'est la page Câbles qui le pose — mais la liste reste nécessaire
  //   pour la DÉCOUPE. Elle se lisait dans les options du champ supprimé : un champ servant de
  //   réservoir caché aurait survécu à sa propre suppression, et personne n'aurait compris
  //   pourquoi il fallait le garder.
  let fluxVideo = [];           // [{valeur, texte, disabled}] — ce qui est joignable D'ICI
  // Remplit « flux à émettre » depuis la liste déjà chargée, et PRÉ-SÉLECTIONNE ce qui est
  // déjà posé : un champ remis à vide à chaque ouverture ferait effacer le réglage par un
  // exploitant venu changer le format.
  // ★★ ON AFFICHE LE FLUX CÂBLÉ, ON NE LE CHOISIT PLUS (22/10, demandé par le testeur).
  //    C'était un menu, et il prenait la main sur la page Câbles : `appliquer()` renvoyait la
  //    valeur du menu dans `flow`, donc venir régler un format ici reposait le flux du menu
  //    par-dessus celui qu'un câble venait de poser. Deux écrivains pour une même donnée, le
  //    second gagnant sans un mot — et c'est ainsi qu'une sortie s'est retrouvée avec la vidéo
  //    d'un flux et l'audio d'un autre.
  //    Le câblage est le SEUL mécanisme de routage ; la console le montre, comme elle montre
  //    déjà le flux produit d'une entrée. Une sortie sans câble le dit, au lieu de laisser
  //    croire qu'il y a un choix à faire ici.
  function remplirFluxSortie(sor) {
    const cible = q("flux-sortie");
    if (!cible) return;
    const flux = (sor && sor.flow) || "";
    cible.textContent = flux || T("flux.non_cable", "aucun — à câbler dans la page Câbles");
  }

  // ★ LA PRISE D'ABORD, LE NOM TECHNIQUE ENSUITE (21/10, signalé en recette). La liste
  //   affichait « decklink-io_7 — DeckLink Quad 2 SDI 8 », et l'exploitant a lu une erreur de
  //   numéro : le 7 n'est pas la prise, c'est le rang de l'entrée au jour où elle a été déclarée.
  //   Ce nom est FIGÉ — l'identité du flux sur le bus en dérive, et le renommer couperait tout ce
  //   qui y est câblé. On ne le corrige donc pas : on le met à sa place, derrière ce qu'on lit.
  function libelleFlux(f, nom) {
    return f.label ? f.label + " (" + nom + ")" : nom;
  }

  async function chargerFluxDisponibles() {
    // ⚠ CE FAUX `select` N'EST PAS UN ÉLÉMENT DU DOM : il accumule des options en mémoire avant
    //   de remplir les vraies listes. Son champ s'appelle `options` et PAS `children`, pour que
    //   le contrôle statique du banc — qui interdit les méthodes de tableau sur une collection
    //   du DOM — n'ait pas à deviner lequel des deux est réel. Un contrôle qui doit distinguer
    //   deux choses homonymes finit par se tromper dans un sens ou dans l'autre.
    const sel = { textContent: "", options: [], appendChild(o) { this.options.push(o); } };
    try {
      const r = await fetch("/api/sources?kind=video", { credentials: "same-origin" });
      const d = r.ok ? await r.json() : [];
      const liste = Array.isArray(d) ? d : (d.sources || []);
      sel.textContent = "";

      // ★ NE PROPOSER QUE CE QUI EST JOIGNABLE D'ICI. Le bus MXL est LOCAL à une machine : un
      //   flux produit sur un autre nœud n'existe pas dans ce domaine. La liste montrait les 42
      //   flux de toute la flotte, et le testeur en a choisi un de dl360-1 (2026-10-02) : sa
      //   sortie a émis À VIDE, « trames répétées », sans que rien n'explique pourquoi. Proposer
      //   un choix qui ne peut pas marcher est une promesse qu'on ne tient pas — je l'avais écrit
      //   pour la liste des connecteurs de découpe, et commis une ligne plus loin.
      //
      //   Le nœud se déduit de NOS PROPRES flux : ceux dont le `vmid` est le nôtre. Pas besoin
      //   d'une route de plus, et la déduction tombe juste dès que ce conteneur produit un flux.
      const moi = liste.find((f) => Number(f.vmid) === Number(VMID));
      const monNoeud = moi ? moi.node : null;
      const local = monNoeud ? liste.filter((f) => f.node === monNoeud) : liste;
      const distants = monNoeud ? liste.filter((f) => f.node !== monNoeud) : [];

      local.forEach((f) => {
        const nom = f.shm || f.flow || "";
        if (!nom) return;
        const o = document.createElement("option");
        o.value = nom;
        o.textContent = libelleFlux(f, nom);
        sel.appendChild(o);
      });
      // ★ LES FLUX DES AUTRES MACHINES SONT OFFERTS, PLUS SEULEMENT ANNONCÉS (19/10).
      //   Ils étaient listés en GRISÉ, avec la mention « le bus mémoire est local, ils ne sont
      //   pas joignables d'ici ». C'était vrai du bus, et faux du système : la réplication
      //   inter-nœuds existe et tourne déjà dans les deux sens. L'exploitant a signalé la
      //   limitation comme arbitraire, et il avait raison — « on ne veut pas que les sources de
      //   la carte elle-même ».
      // ⚠ Les offrir N'A DE SENS que parce que le hook de déploiement PROVISIONNE la
      //   réplication du flux choisi (cf. hooks.py:_repliquer_si_distant). Sans ce filet, on
      //   reviendrait au défaut du 2026-10-02 : une sortie qui émet À VIDE parce que le flux
      //   n'existe pas dans ce domaine. Ne jamais rouvrir l'un sans l'autre.
      if (distants.length) {
        const sep = document.createElement("option");
        sep.disabled = true; sep.value = "";
        sep.textContent = "— " + T("flux.autres_machines", "autres machines (répliqué automatiquement)");
        sel.appendChild(sep);
        distants.forEach((f) => {
          const nom = f.shm || f.flow || "";
          if (!nom) return;
          const o = document.createElement("option");
          o.value = nom;
          // La MACHINE est nommée : choisir un flux distant a un coût (réplication réseau), et
          // l'exploitant doit le savoir avant de cliquer, pas après.
          o.textContent = libelleFlux(f, nom) + " · " + (f.node || "?");
          sel.appendChild(o);
        });
      }
      if (!local.length && !distants.length) {
        note(T("flux.aucun",
               "Aucun flux vidéo n'est disponible sur cette machine — il faut un producteur local."));
      }
      fluxVideo = sel.options.map((o) => ({ valeur: o.value, texte: o.textContent,
                                             disabled: !!o.disabled }));
    } catch (e) {
      // Une liste vide vaut mieux qu'une liste inventée : on le DIT plutôt que de laisser
      // l'exploitant devant un menu vide sans explication.
      note(T("note.flux_indispo", "Impossible de lister les flux existants : ") +
           (e && e.message ? e.message : e));
    }
  }

  // « Hp50 » et consorts sont les noms internes de la carte. On ne les traduit PAS en douce :
  // deviner « 1080p50 » à partir d'un code qu'on ne connaît pas produirait un format faux que
  // le déploiement refuserait, avec un message incompréhensible. On propose, l'humain tranche.
  function modeDetecte(sub) {
    const m = sub && sub.signal && sub.signal.mode_detecte;
    return m ? String(m) : "";
  }

  // La pré-charge est affichée AVEC SON EFFET. « 2 » ne dit rien à l'exploitant ; « 2 trames —
  // environ 200 ms » dit ce qu'il achète. La loi vient de la mesure au banc (2026-10-02) :
  // 120 ms de socle incompressible + 40 ms par trame à 25 i/s.
  function remplirPrecharge(courant) {
    const sel = q("preroll");
    if (!sel) return;
    sel.textContent = "";
    const d = document.createElement("option");
    d.value = "0";
    d.textContent = T("preroll.defaut", "défaut du pilote (3 — environ 240 ms)");
    sel.appendChild(d);
    for (let k = 1; k <= 6; k++) {
      const o = document.createElement("option");
      o.value = String(k);
      o.textContent = k + " " + T("preroll.trames", "trames") + " — " +
                      T("preroll.environ", "environ") + " " + (120 + 40 * k) + " ms";
      sel.appendChild(o);
    }
    sel.value = String(courant || 0);
  }

  // L'attente est offerte AVEC CE QU'ELLE COÛTE ET CE QU'ELLE ÉVITE. « 20 » ne dit rien ; « 20 ms
  // — une trame à 50 i/s » dit où est la limite. Le plafond RÉEL est tenu par le binaire, qui
  // seul connaît la durée d'une trame du mode : au-delà, il ramène la valeur et le DIT dans son
  // journal, puis publie l'attente EFFECTIVE dans ses statistiques. On ne le recalcule pas ici à
  // partir du nom du mode — ce serait deviner, et c'est ce que ce plugin s'interdit.
  function remplirAttenteTranches(courant) {
    const sel = q("slice-wait");
    if (!sel) return;
    sel.textContent = "";
    const d = document.createElement("option");
    d.value = "0";
    d.textContent = T("slicewait.defaut", "défaut du pilote (20 ms)");
    sel.appendChild(d);
    [5, 10, 15, 20, 25, 30, 35, 40].forEach((ms) => {
      const o = document.createElement("option");
      o.value = String(ms);
      o.textContent = ms + " ms";
      sel.appendChild(o);
    });
    sel.value = String(courant || 0);
    const aide = T("slicewait.aide", "Temps laissé au producteur pour finir l'image avant de "
                 + "retomber sur la précédente. Trop court : une image répétée puis une sautée. "
                 + "Plafonné par le binaire à la durée d'une trame.");
    sel.title = aide;
    const l = q("lbl-slice-wait");
    if (l) l.title = aide;
  }

  // ★ LE FORMAT SE CHOISIT, IL NE SE DEVINE PAS (décision du 16/10 : aucune valeur par défaut).
  //   Les modes sont ceux que LA CARTE déclare, avec le nom qu'elle en donne — c'est exactement
  //   cette chaîne que le binaire compare. Avant, le champ était libre et pré-rempli : « le
  //   premier supporté » tombe sur du NTSC SD, et une faute de frappe ne se voyait qu'au
  //   déploiement, sous un « mode introuvable » qui n'aide personne.
  function remplirModes(sub, sor) {
    const sel = q("mode");
    if (!sel) return;
    sel.textContent = "";
    const vide = document.createElement("option");
    vide.value = "";
    vide.textContent = T("mode.choisir", "— à choisir —");
    sel.appendChild(vide);
    const modes = (sub && sub.modes_sortie) || [];
    modes.forEach((m) => {
      const o = document.createElement("option");
      o.value = m;
      // Le format DÉTECTÉ à l'entrée est signalé, pas présélectionné : c'est une indication
      // utile, pas une décision qu'on prend à la place de l'exploitant.
      o.textContent = m + ((modeDetecte(sub) && m.indexOf(modeDetecte(sub)) >= 0)
                           ? "  " + T("mode.detecte", "(détecté à l'entrée)") : "");
      sel.appendChild(o);
    });
    if (!modes.length) {
      const o = document.createElement("option");
      o.disabled = true; o.value = "";
      o.textContent = T("mode.aucun",
        "la carte n'annonce aucun mode de sortie sur cette prise — profil ou duplex ?");
      sel.appendChild(o);
    }
    sel.value = (sor && sor.mode) ? sor.mode : "";
  }

  function remplirDecoupe(sub, sor) {
    const selP = q("key-pid"), selF = q("key-flow");
    if (!selP || !selF) return;
    const subs = sousPeripheriques(inventaire);
    const cfg = configDeployee(etat);
    selP.textContent = "";
    const aucun = document.createElement("option");
    aucun.value = ""; aucun.textContent = T("key.aucune", "aucune (sortie simple)");
    selP.appendChild(aucun);
    subs.forEach((s2) => {
      if (Number(s2.persistent_id) === Number(sub.persistent_id)) return;   // pas elle-même
      if ((s2.duplex || "") === "inactive") return;                         // éteinte : inutile
      if (cfg.entrees.has(Number(s2.persistent_id))) return;                // déjà une entrée
      // ⚠ NI UNE PRISE DÉJÀ EN SORTIE. Révélé par le banc le 19/10 : la liste offrait un
      //   connecteur portant déjà une sortie, ce qui mettrait DEUX sorties sur le même BNC.
      //   L'exclusion des entrées était là, celle des sorties manquait — et rien ne l'aurait
      //   signalé : le déploiement aurait accepté, et deux binaires se seraient disputé la
      //   prise, l'un échouant à l'ouvrir sans que personne sache lequel avait raison.
      if (cfg.sorties.has(Number(s2.persistent_id))) return;
      const o = document.createElement("option");
      o.value = String(s2.persistent_id);
      o.textContent = nomPhysique(s2.persistent_id);
      selP.appendChild(o);
    });
    selP.value = (sor && sor.key_persistent_id) ? String(sor.key_persistent_id) : "";
    // Le flux de découpe se choisit dans la MÊME liste que le flux à émettre : c'est un flux
    // vidéo ordinaire, et le présenter autrement laisserait croire à un format particulier.
    selF.textContent = "";
    fluxVideo.forEach((f) => {
      const c = document.createElement("option");
      c.value = f.valeur; c.textContent = f.texte;
      if (f.disabled) c.disabled = true;
      selF.appendChild(c);
    });
    if (sor && sor.key_flow) selF.value = sor.key_flow;
  }

  /// Canaux audio OUVERTS sur la carte pour une sortie. « suivre le câblage » (valeur vide)
  /// laisse le superviseur décider : deux flux câblés → 16, un seul → 8. C'est le défaut, et
  /// c'est le seul qui ne puisse pas mentir — avant le 21/10 le défaut était 16 en dur, ce qui
  /// ouvrait 16 canaux avec un seul flux de 8 câblé et laissait 9-16 muets sans un mot.
  function remplirAudioSortie(sor) {
    const sel = q("audio-sortie");
    if (!sel) return;
    const lbl = q("lbl-audio-sortie");
    if (lbl) lbl.textContent = T("editeur.audio_sortie", "Audio embarqué");
    const aide = T("editeur.audio_sortie_aide", "");
    if (aide) { sel.title = aide; if (lbl) lbl.title = aide; }
    const choix = [["", T("editeur.audio_sortie_auto", "suivre le câblage")],
                   ["2", T("editeur.audio_sortie_2", "2 canaux (1 flux)")],
                   ["8", T("editeur.audio_sortie_8", "8 canaux (1 flux)")],
                   ["16", T("editeur.audio_sortie_16", "16 canaux (2 flux de 8)")]];
    sel.textContent = "";
    choix.forEach(([v, t]) => {
      const o = document.createElement("option");
      o.value = v; o.textContent = t;
      sel.appendChild(o);
    });
    const actuel = Number((sor && sor.audio_channels) || 0);
    sel.value = (actuel === 2 || actuel === 8 || actuel === 16) ? String(actuel) : "";
  }

  function majChamps() {
    const r = q("role").value;
    q("bloc-flux-fixe").hidden = (r !== "entree");
    q("bloc-flux-sel").hidden = (r !== "sortie");
    q("bloc-audio").hidden = (r !== "entree");
    q("bloc-audio-sortie").hidden = (r !== "sortie");
    q("bloc-anc").hidden = (r !== "entree");
    q("bloc-mode").hidden = (r !== "sortie");
    q("bloc-preroll").hidden = (r !== "sortie");
    q("bloc-slice-wait").hidden = (r !== "sortie");
    q("bloc-key").hidden = (r !== "sortie");
    q("bloc-key-flow").hidden = (r !== "sortie") || !(q("key-pid").value || "");
  }

  function fermerEditeur() {
    choisi = null;
    q("editeur").hidden = true;
    dessiner();
  }

  // Construit les DEUX listes complètes : on poste l'état voulu, jamais un delta. Un delta
  // supposerait que la base n'a pas bougé entre la lecture et l'envoi.
  // ★ LA CARTE ET LE NUMÉRO DE PRISE VOYAGENT AVEC LA DÉCLARATION (20/10, demandé en recette).
  //   Les ports de la page Câbles s'appelaient « Entrée SDI 1, 2, 3… » — une simple
  //   incrémentation dans l'ordre de la liste, qui ne dit NI de quelle carte il s'agit, NI quel
  //   connecteur. Avec deux cartes dans la machine, « SDI 2 » ne désigne plus rien.
  //   Le modèle ne peut venir que d'ICI : il est lu du SDK, dans le conteneur, et l'orchestrateur
  //   ne le connaît pas. On le joint donc à la déclaration, avec le rang de la prise DANS SA
  //   CARTE — le même que celui affiché sur la pastille.
  function reperesDe(pid) {
    const groupes = grouperParCarte(sousPeripheriques(inventaire));
    for (const g of groupes) {
      const i = g.findIndex((x) => Number(x.persistent_id) === Number(pid));
      if (i >= 0) return { carte: (g[0] && g[0].model) || "", prise: i + 1 };
    }
    return {};
  }

  // ★★ LE NUMÉRO DE PRISE EST CELUI DE L'EXPLOITANT, PAS CELUI DE LA CARTE (22/10).
  //    Cette fonction écrasait `prise` par le rang du connecteur dans l'inventaire, à CHAQUE
  //    application. Or ce rang n'est pas le numéro sérigraphié sur le châssis : sur la Quad 2,
  //    la carte annonce ses sous-périphériques dans un ordre ENTRELACÉ entre ses deux
  //    périphériques PCI, si bien que son « Quad 5 » est le connecteur 2 du panneau.
  //    Conséquence vécue le jour même : j'ai « réparé » huit numéros en les alignant sur la
  //    carte, détruisant ceux que le testeur avait relevés AU CÂBLE, et la page Câbles s'est
  //    mise à désigner les mauvaises prises. C'est la même faute que le champ « flux à
  //    émettre » retiré le même jour : la console remplaçait une donnée de l'exploitant par sa
  //    propre dérivation, et gagnait en silence.
  // ⚠ LE MODÈLE, LUI, VIENT BIEN DE LA CARTE : l'orchestrateur ne le connaît pas, il n'est
  //   lisible que du conteneur. On garde donc `carte` dérivé, et on PRÉSERVE `prise`.
  //   Le rang de la carte ne sert que de valeur initiale pour un connecteur jamais déclaré :
  //   mieux vaut une proposition à corriger que rien du tout, et l'exploitant la corrige.
  function avecReperes(e) {
    const r = reperesDe(e.persistent_id);
    if (!r.carte) return e;
    const cfg = configDeployee(etat);
    const pid = Number(e.persistent_id);
    const avant = cfg.sorties.get(pid) || cfg.entrees.get(pid) || {};
    const declaree = (e.prise != null && e.prise !== "") ? e.prise
                   : ((avant.prise != null && avant.prise !== "") ? avant.prise : null);
    return Object.assign({}, e, { carte: r.carte,
                                  prise: declaree != null ? declaree : r.prise });
  }

  function listesVoulues() {
    const cfg = configDeployee(etat);
    const entrees = [], sorties = [];
    // On rafraîchit les repères de TOUTES les prises, pas seulement de celle qu'on édite :
    // sinon les ports déclarés avant cette version garderaient leur libellé incrémenté.
    cfg.entrees.forEach((v, k) => { if (k !== choisi) entrees.push(avecReperes(v)); });
    cfg.sorties.forEach((v, k) => { if (k !== choisi) sorties.push(avecReperes(v)); });
    const r = q("role").value;
    if (r === "entree") {
      entrees.push(avecReperes({ persistent_id: choisi, flow: q("flux-fixe").textContent.trim(),
                     audio: Number(q("audio").value), anc: !!q("anc").checked }));
    } else if (r === "sortie") {
      // ★ LE FLUX À ÉMETTRE NE SE RÈGLE PAS ICI (19/10). C'est la page Câbles qui le pose : le
      //   manifeste déclare déjà `consumes` sur `outputs.flow`, donc ce champ était un SECOND
      //   endroit où écrire la même valeur — la faute même qui, ce matin, faisait afficher une
      //   sortie morte. On REPREND le flux déjà câblé au lieu de le redemander ; l'omettre
      //   l'effacerait au premier réglage de format, et la sortie se tairait sans rien dire.
      // Le champ fait foi ; s'il est vide on CONSERVE ce qui était posé plutôt que de
      // l'effacer — un réglage de format ne doit pas rendre une sortie muette.
      // ⚠ LE FLUX VIENT DU CÂBLAGE, JAMAIS DE LA CONSOLE (22/10). On le relit tel qu'il est
      //   posé et on le renvoie à l'identique : c'est la seule façon de régler un format ou une
      //   pré-charge sans toucher au routage. L'omettre l'effacerait au premier réglage, et la
      //   sortie se tairait sans rien dire.
      const dejaCable = (cfg.sorties.get(choisi) || {}).flow || "";
      const o = { persistent_id: choisi, flow: dejaCable,
                  mode: q("mode").value.trim() };
      const pre = Number(q("preroll").value || 0);
      if (pre > 0) o.preroll = pre;
      const att = Number(q("slice-wait").value || 0);
      if (att > 0) o.slice_wait_ms = att;
      // Vide = « suivre le câblage » : on n'écrit RIEN, et le superviseur déduit des flux
      // câblés. Écrire 0 serait un réglage, pas une absence de réglage.
      const ac = Number(q("audio-sortie").value || 0);
      if (ac === 2 || ac === 8 || ac === 16) o.audio_channels = ac;
      const kp = (q("key-pid").value || "").trim();
      const kf = (q("key-flow").value || "").trim();
      if (kp && kf) { o.key_persistent_id = Number(kp); o.key_flow = kf; }
      sorties.push(avecReperes(o));
    }
    return { inputs: entrees, outputs: sorties };
  }

  // L'éditeur d'une prise ne redéploie plus : il ajoute au lot (cf. `lot`).
  function ajouterAuLot() {
    if (envoiEnCours) return;
    const r = q("role").value;
    // ⚠⚠ ON NE REFUSE PAS une sortie sans flux, et c'est DÉLIBÉRÉ. J'ai posé ici un contrôle
    //    « choisissez d'abord le flux » le 22/10, et il recréait un cercle vicieux déjà résolu
    //    côté hooks le 19/10 : pour câbler une source dans la page Câbles il faut un PORT, le
    //    port naît de la sortie déclarée, donc exiger le flux avant la déclaration interdit de
    //    partir d'une prise vide — on ne peut plus jamais créer une sortie.
    //    « Connecteur déclaré en sortie, source pas encore câblée » est un état légitime : le
    //    port apparaît, l'exploitant câble, le travailleur démarre. Le MODE reste obligatoire
    //    (contrôle juste en dessous) parce qu'il n'a pas de défaut acceptable et qu'il n'est
    //    pas une conséquence du câblage.

    if (r === "sortie" && !(q("mode").value || "").trim()) {
      note(T("note.mode_requis", "Choisissez le format de sortie — il n'a pas de valeur par défaut."));
      return;
    }
    const voulu = listesVoulues();
    if (!lot) {
      const tab = (x) => Array.isArray(x) ? x : [];
      const src = config || etat || {};
      lotOrigine = { inputs: tab(src.inputs), outputs: tab(src.outputs) };
    }
    lot = voulu;
    config = Object.assign({}, config || {}, voulu);
    aConfirmer = false;
    plafondAccepte = false;      // le lot a changé : un consentement donné avant ne vaut plus
    fermerEditeur();
    majLot();
  }

  // Ce qui DÉCRIT une prise dans une liste, sans les repères (carte, numéro) que la page recalcule.
  function descriptionsParPrise(p) {
    const out = new Map();
    const pose = (role, x) => {
      if (!x || x.persistent_id == null) return;
      const d = Object.assign({}, x);
      delete d.carte; delete d.prise;
      const cles = Object.keys(d).sort();
      out.set(Number(x.persistent_id), role + JSON.stringify(cles.map((k) => [k, d[k]])));
    };
    ((p && p.inputs) || []).forEach((x) => pose("e", x));
    ((p && p.outputs) || []).forEach((x) => pose("s", x));
    return out;
  }

  function changementsDuLot() {
    if (!lot) return 0;
    const a = descriptionsParPrise(lotOrigine), b = descriptionsParPrise(lot);
    const pids = new Set([...a.keys(), ...b.keys()]);
    let n = 0;
    pids.forEach((k) => { if (a.get(k) !== b.get(k)) n += 1; });
    return n;
  }

  function noteLot(txt) {
    const n = q("lot-note");
    if (n) { n.textContent = txt || ""; n.hidden = !txt; }
  }

  let lotEstimation = 0;          // numéro de la dernière estimation demandée (les réponses en retard sont jetées)
  async function majLot() {
    const bloc = q("lot");
    if (!bloc) return;
    const n = changementsDuLot();
    if (lot && n === 0) {        // revenu à l'identique : il n'y a plus rien à appliquer
      abandonnerLot();
      return;
    }
    bloc.hidden = !lot;
    if (!lot) return;
    const btn = q("lot-appliquer");
    const resume = q("lot-resume");
    const base = T("lot.resume", "{n} changement(s) en attente.").replace(/\{n\}/g, String(n));
    resume.textContent = base;
    btn.disabled = false;
    const num = ++lotEstimation;
    let est = null;
    try {
      const r = await fetch(cfgApi("/cores_estimate"), {
        method: "POST", credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ params: lot }),
      });
      est = r.ok ? await r.json() : null;
    } catch (e) { est = null; }
    if (num !== lotEstimation || !lot) return;
    if (!est) return;            // orchestrateur sans estimation : on applique sans chiffrer
    // ★ LE COÛT SE DIT AVANT D'APPLIQUER : la carte reçoit des cœurs physiques entiers, à la mesure
    //   de ses prises. Si le nœud ne les a pas, on REFUSE ici plutôt que de la démarrer à moitié
    //   sur le pool partagé — et on nomme qui tient les cœurs, pour savoir quoi libérer.
    const coeurs = est.forced
      ? T("lot.coeurs_fixes", "Cœurs fixés à {a} pour ce conteneur.").replace(/\{a\}/g, String(est.wanted))
      : T("lot.coeurs", "Cœurs de la carte : {de} → {a}.").replace(/\{de\}/g, String(est.current))
          .replace(/\{a\}/g, String(est.wanted));
    resume.textContent = base + " " + coeurs;
    if (!est.fits) {
      const tenants = (est.holders || []).map((h) => (h.hostname || ("#" + h.vmid)) + " (" + h.cores + ")").join(", ");
      noteLot(T("lot.refus", "Le nœud n'a que {dispo} cœur(s) physique(s) libre(s) pour {a} : {tenants}. "
                + "Libérez des cœurs ou retirez des prises.")
        .replace(/\{dispo\}/g, String(est.available)).replace(/\{a\}/g, String(est.wanted))
        .replace(/\{tenants\}/g, tenants || "—"));
      btn.disabled = true;
    }
  }

  function abandonnerLot() {
    if (lotOrigine) config = Object.assign({}, config || {}, lotOrigine);
    lot = null;
    lotOrigine = null;
    aConfirmer = false;
    plafondAccepte = false;
    noteLot("");
    const bloc = q("lot");
    if (bloc) bloc.hidden = true;
    if (choisi !== null) fermerEditeur(); else dessiner();
  }

  async function appliquerLot() {
    if (envoiEnCours || !lot) return;
    const params = lot;
    // ★★ LA 8ᵉ SORTIE N'EST PAS FONCTIONNELLE, ET ÇA DOIT SE DIRE AVANT (22/10, décidé avec
    //    l'utilisateur). Au-delà de 7 sorties simultanées, des prises passent en retard
    //    permanent et la carte écarte leur AUDIO : l'image reste, le son disparaît, et aucun
    //    compteur ne bronche — 50 i/s et genlock verrouillé sur toutes. Le testeur a cherché
    //    la panne une journée entière dans ses câbles, ses canaux et ses flux.
    // ⚠ ON AVERTIT, ON NE REFUSE PAS : la cause n'est pas isolée, donc le chiffre peut bouger
    //   avec le format ou le nombre d'entrées, et figer un refus sur une limite qu'on ne
    //   comprend pas serait pire. Mais l'avertissement passe par le MÊME geste de consentement
    //   que le serveur réclame ailleurs — un second clic — parce qu'une note qui défile pendant
    //   qu'on applique n'est pas un avertissement, c'est une décoration.
    {
      const surcharge = cartesEnSurcharge(params.outputs || []);
      if (surcharge.length && !plafondAccepte) {
        const x = surcharge[0];
        noteLot(T("note.plafond_sorties",
          "{c} porterait {n} sorties : au-delà de {max} sur une même carte, des sorties "
          + "émettent en retard et LEUR SON DISPARAÎT. Ce n'est pas encore fonctionnel.")
          .replace(/\{c\}/g, x.carte || T("avert.cette_carte", "cette carte"))
          .replace(/\{n\}/g, String(x.n))
          .replace(/\{max\}/g, String(PLAFOND_SORTIES_PAR_CARTE))
          + " " + T("note.confirmer_suite", "Cliquez à nouveau sur Appliquer pour confirmer."));
        plafondAccepte = true;
        return;
      }
    }
    envoiEnCours = true;
    q("lot-appliquer").disabled = true;
    noteLot(T("note.envoi", "Enregistrement et redéploiement…"));
    try {
      const rep = await fetch(cfgApi("/plugin_config"), {
        method: "POST", credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(aConfirmer ? { params: params, confirm: true } : { params: params }),
      });
      const d = await rep.json().catch(() => ({}));
      // ★ Le serveur peut EXIGER une confirmation (redéploiement coupant des flux). On la relaie
      //   au lieu de la contourner : le second clic vaut consentement, et le motif est celui du
      //   serveur, pas un texte que la page aurait inventé de son côté.
      if (rep.status === 409 && d && d.needs_confirm) {
        noteLot((d.reason || T("note.confirmer", "Cette modification interrompt des flux.")) + " " +
             T("note.confirmer_suite", "Cliquez à nouveau sur Appliquer pour confirmer."));
        aConfirmer = true;
        return;
      }
      if (!rep.ok) throw new Error(d.error || ("HTTP " + rep.status));
      // Accepté, mais PAS encore écrit (cf. `enAttente`). On garde les dispositions : `params`
      // ne porte que les prises, et les remplacer par lui les effaçait de la page.
      config = Object.assign({}, config || {}, params);
      retenirEnvoi(params);
      afficherAttente(texteAttente());
      aConfirmer = false;
      plafondAccepte = false;      // un consentement vaut pour CE geste, pas pour le suivant
      lot = null;
      lotOrigine = null;
      noteLot("");
      q("lot").hidden = true;
      if (choisi !== null) fermerEditeur(); else dessiner();
    } catch (e) {
      noteLot(T("note.echec", "Échec : ") + (e && e.message ? e.message : e));
    } finally {
      envoiEnCours = false;
      const b = q("lot-appliquer");
      if (b && lot) b.disabled = false;
    }
  }

  async function lireConfig() {
    if (!editable()) return null;
    const r = await fetch(cfgApi("/plugin_config"), { credentials: "same-origin" });
    if (!r.ok) return null;
    const d = await r.json();
    const p = (d && d.params) || {};
    const tab = (x) => Array.isArray(x) ? x : [];
    // ⚠ CE LECTEUR NE GARDAIT QUE LES ENTRÉES ET LES SORTIES. Les dispositions enregistrées
    //   partaient donc bien au serveur — elles y étaient — mais la console les JETAIT à la
    //   relecture, et la liste déroulante restait vide. L'exploitant voyait « enregistrée »
    //   puis plus rien, ce qui se lit comme un enregistrement qui n'a pas pris.
    //   C'est le troisième endroit aujourd'hui où une recopie champ par champ perd ce qu'on
    //   vient d'ajouter — après la pré-charge et le flux audio côté superviseur. Une liste
    //   tenue à la main se périme à chaque nouveau champ.
    return { inputs: tab(p.inputs), outputs: tab(p.outputs),
             layouts: (p.layouts && typeof p.layouts === "object") ? p.layouts : {} };
  }

  // Ce que la page DESSINE d'une configuration : le rôle de chaque prise et, pour une sortie,
  // son flux et son format. Le serveur normalise le reste (repères, entiers) : comparer l'objet
  // entier ne conclurait jamais, et l'attente ne finirait qu'à l'expiration.
  function signatureRoles(p) {
    const ent = ((p && p.inputs) || []).map((e) => String(e.persistent_id)).sort();
    const sor = ((p && p.outputs) || []).map((o) => [o.persistent_id, o.flow || "", o.mode || "",
                                                    o.key_persistent_id || ""].join("|")).sort();
    return JSON.stringify([ent, sor]);
  }

  function retenirEnvoi(p) {
    enAttente = { sig: signatureRoles(p), inputs: p.inputs || [], outputs: p.outputs || [],
                  depuis: Date.now() };
  }

  // La base relue, corrigée de ce qui est encore en route vers elle.
  function avecEnvoiEnCours(lu) {
    if (!enAttente || !lu) return lu;
    if (signatureRoles(lu) === enAttente.sig || Date.now() - enAttente.depuis > ATTENTE_MAX_MS) {
      enAttente = null;
      return lu;
    }
    return Object.assign({}, lu, { inputs: enAttente.inputs, outputs: enAttente.outputs });
  }

  async function rafraichir() {
    const err = EL.querySelector('[data-dl="erreur"]');
    try {
      const [inv, st] = await Promise.all([lire("/inventory"), lire("/state")]);
      if (inv && inv.erreur) throw new Error(inv.erreur);
      inventaire = inv; etat = st;
      // Pendant une édition on NE rafraîchit PAS la configuration : l'exploitant est en train de
      // la modifier, et l'écraser sous ses doigts lui ferait perdre sa saisie.
      // Un lot en préparation non plus : le relire l'effacerait.
      if (choisi === null && !lot) config = avecEnvoiEnCours(await lireConfig()) || config;
      // L'attente se DIT : sans ça, la carte qui n'a pas encore bougé contredit la page, et on
      // ne sait pas laquelle croire.
      err.hidden = true;
      // La lecture a réussi : toute attente en cours est TERMINÉE. Ne pas remettre ce compteur
      // à zéro ferait franchir le plafond à la première coupure suivante, aussi brève soit-elle.
      masquerConteneur();
      afficherAttente(enAttente ? texteAttente() : "");
      dessiner();
    } catch (e) {
      // ★ UN REDÉPLOIEMENT N'EST PAS UNE PANNE. Pendant les ~20 s où le conteneur redémarre —
      //   redémarrage que CETTE PAGE vient de déclencher — le relais rend 502 ou 404. Le testeur
      //   a vu « inventory 502 » s'afficher, puis disparaître, puis son réglage s'appliquer :
      //   un message technique alarmant pour un fonctionnement normal. On nomme ce qui se passe.
      const msg = (e && e.message) ? e.message : String(e);
      const redeploie = /\b(502|503|504|404)\b/.test(msg) || /Failed to fetch/i.test(msg);
      if (redeploie) {
        if (!injoignableDepuis) injoignableDepuis = Date.now();
        const ecoule_s = (Date.now() - injoignableDepuis) / 1000;
        err.hidden = true;
        if (ecoule_s <= PLAFOND_INJOIGNABLE_S) {
          // Même avis, même place : c'est la même attente, vue d'un autre moment. Le temps
          // écoulé en fait partie — sans lui on ne distingue pas « ça avance » de « c'est mort ».
          afficherAttente(T("erreur.redeploiement",
            "Redéploiement en cours — le conteneur redémarre, l'affichage revient dans quelques secondes.")
            + " " + T("attente.depuis", "({s} s)").replace("{s}", String(Math.round(ecoule_s))));
        } else {
          // ★ LE PLAFOND EST FRANCHI : ce n'est plus un redéploiement. On retire l'avis
          //   d'attente — le laisser à côté du vrai état ferait coexister deux affirmations
          //   contradictoires — et on va lire ce que la base dit du conteneur.
          afficherAttente("");
          await direEtatConteneur(ecoule_s);
        }
      } else {
        err.textContent = T("erreur", "Lecture impossible : ") + msg;
        err.hidden = false;
      }
    }
  }

  // ── Attente BORNÉE ──────────────────────────────────────────────────────────────────────
  // Un redéploiement prend 40 à 80 s (mesuré le 21/10). Au-delà du double, ce n'en est plus un :
  // le conteneur est arrêté, absent, ou l'orchestrateur ne le joint plus. Sans ce plafond,
  // l'attente ne se termine JAMAIS — c'est ce que Cyril a eu sous les yeux le 30/10 sur #228,
  // mort 4 s après sa création faute de pilote et annoncé « de retour dans quelques secondes »
  // pendant onze minutes.
  const PLAFOND_INJOIGNABLE_S = 150;
  const PERIODE_ETAT_MS = 10000;        // l'état du conteneur se lit hors du proxy : pas à 2 s
  let injoignableDepuis = 0, etatConteneurLu = 0, piloteManque = false;

  function masquerConteneur() {
    const b = q("bloc-conteneur");
    if (b) b.hidden = true;
    injoignableDepuis = 0;
    etatConteneurLu = 0;
  }

  function direConteneur(txt, demarrable) {
    const b = q("bloc-conteneur"), t = q("conteneur-texte"), bo = q("conteneur-demarrer");
    if (!b || !t) return;
    t.textContent = txt;
    b.hidden = false;
    if (bo) {
      // Jamais en mode public : `/api/containers/<vmid>/restart` est hors du proxy de plugin et
      // exige `containers.deploy`. Un bouton qui ne peut que rendre 401 est pire qu'absent.
      bo.hidden = !(demarrable && editable());
      bo.textContent = T("conteneur.demarrer", "Démarrer le conteneur");
      bo.onclick = () => demarrerConteneur();
    }
  }

  async function demarrerConteneur() {
    const bo = q("conteneur-demarrer");
    if (bo) bo.hidden = true;
    try {
      const r = await fetch("/api/containers/" + VMID + "/restart",
                            { method: "POST", credentials: "same-origin",
                              headers: { "Content-Type": "application/json" }, body: "{}" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      // On REPART l'attente : le démarrage est maintenant un vrai redéploiement, déclenché
      // d'ici, et c'est le seul cas où « revient dans quelques secondes » est exact.
      injoignableDepuis = Date.now();
      etatConteneurLu = 0;
      direConteneur(T("conteneur.demarrage", "Démarrage demandé — le conteneur repart."), false);
    } catch (e) {
      direConteneur(T("conteneur.echec", "Le démarrage a été refusé : ")
                    + (e && e.message ? e.message : String(e)), true);
    }
  }

  // Ce que dit la BASE du conteneur, quand la page ne le joint plus depuis trop longtemps.
  async function direEtatConteneur(ecoule_s) {
    const n = Math.round(ecoule_s);
    if (!editable()) {
      // Page publique : aucun appel hors du proxy. On ne peut que constater, et le dire.
      // `true` en connaissance de cause : c'est bien un conteneur qu'on voudrait démarrer.
      // C'est `direConteneur` — et lui SEUL — qui sait qu'on ne peut pas l'offrir ici. Deux
      // endroits qui décident la même chose finissent par en décider deux différentes.
      direConteneur(T("conteneur.public", "Ce flux est indisponible : le conteneur ne répond "
                      + "plus depuis {s} s.").replace("{s}", String(n)), true);
      return;
    }
    if (Date.now() - etatConteneurLu < PERIODE_ETAT_MS) return;
    etatConteneurLu = Date.now();
    let moi = null;
    try {
      const r = await fetch("/api/containers", { credentials: "same-origin" });
      if (r.ok) {
        const d = await r.json();
        const liste = Array.isArray(d) ? d : (d.containers || []);
        moi = liste.find((c) => Number(c.vmid) === Number(VMID)) || null;
      }
    } catch (e) { moi = null; }
    if (!moi) {
      direConteneur(T("conteneur.inconnu", "Ce conteneur n'est plus dans la base de "
                      + "l'orchestrateur : la page n'a plus rien à interroger."), false);
      return;
    }
    const st = String(moi.status || "");
    const voulu = String(moi.desired_state || "");
    const arrete = (st !== "running");
    let txt;
    // ★ `awaiting_driver` N'EST PAS UNE PANNE, c'est un état VOULU — le cœur a délibérément
    //   choisi de ne pas lancer ce conteneur parce que le pilote manque (host_driver_gate).
    //   Le dire « ARRÊTÉ, et rien ne le relèvera tout seul » serait deux fois faux : personne
    //   ne l'a arrêté, et quelque chose le relèvera.
    if (st === "attente_pilote" || st === "awaiting_driver") {
      // Aucun bouton : le geste utile est d'installer le pilote, et le bandeau rouge juste
      // au-dessus le propose déjà. Un second bouton qui ne ferait que retomber dans la même
      // attente donnerait l'impression d'un démarrage qui échoue.
      direConteneur(T("conteneur.attente_pilote",
        "Ce conteneur ATTEND SON PILOTE : il n'a pas été lancé, délibérément, parce que le "
        + "pilote manque sur ce nœud. Il démarrera de lui-même au redémarrage du nœud, une fois "
        + "le pilote installé — il n'y a aucun geste à faire ici."), false);
      return;
    }
    if (arrete) {
      txt = T("conteneur.arrete", "Le conteneur est ARRÊTÉ ({st}) depuis {s} s, et rien ne le "
              + "relèvera tout seul.").replace("{st}", st || "?").replace("{s}", String(n));
      if (voulu === "running") {
        // L'écart entre VOULU et OBSERVÉ est l'information : personne ne l'a arrêté.
        txt += " " + T("conteneur.voulu_demarre", "Il est pourtant censé tourner : il s'est "
                       + "arrêté de lui-même.");
      }
      if (moi.status_cause) txt += " " + String(moi.status_cause);
      if (piloteManque) {
        txt += " " + T("conteneur.cause_pilote", "La cause est très probablement le pilote "
                       + "absent, signalé ci-dessus : sans lui le binaire de capture ne trouve "
                       + "aucune carte et s'arrête quelques secondes après le démarrage.");
      }
    } else {
      txt = T("conteneur.muet", "Le conteneur est déclaré démarré mais ne répond plus depuis "
              + "{s} s.").replace("{s}", String(n));
    }
    direConteneur(txt, true);
  }

  // Le temps écoulé fait partie du message : un redéploiement prend 40 à 80 s (mesuré le
  // 21/10), et sans compteur on ne distingue pas « ça avance » de « c'est bloqué ».
  function texteAttente() {
    const s = Math.max(0, Math.round((Date.now() - enAttente.depuis) / 1000));
    return T("attente.redeploiement",
      "Redéploiement en cours — la carte n'a pas encore pris ce réglage. La page montre ce que vous avez demandé.")
      + " " + T("attente.depuis", "({s} s)").replace("{s}", String(s));
  }

  // ── Légende des couleurs ────────────────────────────────────────────────────────────────
  // Les entrées sont déclarées ici, UNE fois, et chacune porte les attributs qu'une vraie prise
  // porterait : c'est le CSS du ruban qui les colorie. Aucune couleur n'apparaît dans ce
  // fichier — si une légende pouvait se tromper de couleur, elle ne vaudrait rien.
  const LEGENDE = [
    { role: "entree",  cle: "legende.entree",   fb: "entrée SDI" },
    { role: "sortie",  cle: "legende.sortie",   fb: "sortie SDI" },
    { role: "decoupe", cle: "legende.decoupe",  fb: "sortie de découpe (key)" },
    { role: "ref",     cle: "legende.ref",      fb: "référence (genlock)" },
    { role: "libre",   cle: "legende.libre",    fb: "libre, rien d'affecté" },
    { role: "inactif", cle: "legende.inactif",  fb: "éteinte par le profil de la carte" },
    // Le halo est un AXE À PART : il se superpose à n'importe quel rôle. D'où `signal: "1"`
    // sur une prise au rôle neutre — montrer un halo sur une prise colorée ferait croire que
    // les deux marques vont ensemble.
    { role: "libre", signal: "1", cle: "legende.signal", fb: "un signal est présent (halo)" },
  ];

  function dessinerLegende() {
    const ul = q("legende");
    if (!ul || ul.childElementCount) return;      // posée une fois, elle ne change pas
    LEGENDE.forEach((e) => {
      const li = document.createElement("li");
      const prise = document.createElement("span");
      prise.className = "dl-prise";
      prise.dataset.role = e.role;
      prise.dataset.signal = e.signal || "0";
      prise.setAttribute("aria-hidden", "true");  // la couleur est redite en texte juste après
      const bnc = document.createElement("span");
      bnc.className = "dl-bnc";
      prise.appendChild(bnc);
      const txt = document.createElement("span");
      txt.textContent = T(e.cle, e.fb);
      li.appendChild(prise);
      li.appendChild(txt);
      ul.appendChild(li);
    });
  }

  function afficherAttente(txt) {
    const a = q("attente");
    if (!a) return;
    a.textContent = txt || "";
    a.hidden = !txt;
  }

  // ── MICROLOGICIEL DE LA CARTE ────────────────────────────────────────────────────────
  // Une DeckLink dont le micrologiciel ne correspond pas au pilote est ACTIVÉE par le noyau
  // mais INVISIBLE au SDK : elle est dans le châssis et rien ne la voit. C'est arrivé deux
  // fois, et les deux fois il a fallu un shell. Le travail se fait sur l'HÔTE (le conteneur
  // n'a pas l'outil), donc on parle au pilote hôte du nœud, pas à notre proxy de plugin.
  let noeudId = null, firmwareEnCours = false, minuteurMicro = 0;
  //: Version du pilote Desktop Video du NŒUD. Affichée dans l'en-tête, sans rien à ouvrir :
  //: c'est la première chose qu'on demande quand une carte se comporte mal, et elle était
  //: enterrée dans un repli. Elle vaut pour la machine, pas pour une carte — d'où l'en-tête
  //: du panneau et non le titre d'un bandeau.
  let versionPilote = "";

  async function monNoeudId() {
    if (noeudId !== null) return noeudId;
    // Le nœud ne se devine pas : on le lit sur NOTRE conteneur. Une seule fois, puis en cache.
    const r = await fetch("/api/containers", { credentials: "same-origin" });
    if (!r.ok) return null;
    const d = await r.json();
    const liste = Array.isArray(d) ? d : (d.containers || []);
    const moi = liste.find((c) => Number(c.vmid) === Number(VMID));
    noeudId = moi && moi.node_id ? Number(moi.node_id) : null;
    return noeudId;
  }

  function phraseMicro(plan) {
    const p = (plan && plan.params) || {};
    let txt = TC("settings.hostdrv.blackmagic.plan." + ((plan && plan.code) || ""), "");
    if (!txt) return "";
    Object.keys(p).forEach((k) => { txt = txt.split("{" + k + "}").join(String(p[k])); });
    return txt;
  }

  // ── Pilote absent ──────────────────────────────────────────────────────────────────────
  // Les états où le pilote Desktop Video n'est PAS exploitable. `no_tool` en fait partie : le
  // paquet est là mais l'outil manque, donc l'installation est à moitié faite — l'annoncer
  // « aucune carte » enverrait chercher la panne dans le châssis.
  const PILOTE_ABSENT = ["install_needs_file", "missing_prereqs", "install", "no_tool"];

  // Écrit dans TOUS les journaux présents. Il y en a deux — celui du bandeau et celui du bloc
  // replié — et le travail peut être lancé depuis l'un ou l'autre : n'alimenter que le second
  // ferait d'une installation lancée en haut de page une opération muette.
  function jLog(txt) {
    ["pilote-journal", "firmware-journal"].forEach((nom) => {
      const jr = q(nom);
      if (!jr) return;
      jr.hidden = false;
      jr.textContent += txt;
    });
  }

  function jVider() {
    ["pilote-journal", "firmware-journal"].forEach((nom) => {
      const jr = q(nom);
      if (jr) { jr.hidden = false; jr.textContent = ""; }
    });
  }

  // Les quatre étapes, dans l'ordre réel. Rendre l'étape COURANTE visible est tout l'intérêt :
  // déposer l'archive ne charge pas les modules, et le redémarrage du nœud ne se devine pas.
  function etapesPilote(st, f) {
    const paquet = !!(f && f.package);
    const modules = f ? Number(f.modules || 0) : 0;
    const fichier = !!(f && (f.files || []).length);
    const etats = [
      { cle: "depot",   faite: fichier || paquet },
      { cle: "install", faite: paquet },
      { cle: "reboot",  faite: paquet && modules > 0 },
      { cle: "relance", faite: false }
    ];
    // La première non faite est celle où l'on est. `missing_prereqs` est la seule exception :
    // le fichier est déposé mais l'installation est IMPOSSIBLE d'ici (il faut apt sur le nœud),
    // donc on désigne l'installation sans prétendre qu'un clic la débloquerait.
    const i = etats.findIndex((e) => !e.faite);
    if (i >= 0) etats[i].ici = true;
    if (st === "missing_prereqs") { etats.forEach((e) => { e.ici = false; }); etats[1].ici = true; }
    return etats;
  }

  // ★ LA PORTE N'EST PAS ICI (2026-10-31, demandé par Cyril). Le pilote Desktop Video
  //   s'installe sur l'HÔTE, par l'agent du nœud — ce plugin ne fait que dessiner l'écran
  //   par-dessus le relais `/api/nodes/<id>/host-drivers/blackmagic/*`. Jusqu'ici cette page
  //   était pourtant le SEUL endroit d'où on pouvait le faire : Réglages n'offrait la boîte que
  //   pour `nvidia`. Donc la seule porte pour installer le pilote était la page d'un conteneur
  //   qui, pour exister, a besoin du pilote. Cyril a tourné dans ce cercle sur une machine
  //   neuve le 30/10. La boîte `blackmagic` est maintenant offerte dans Réglages → Nœuds, sans
  //   condition, et on y renvoie d'ici.
  // ⚠ Le lien est construit en DOM, pas en innerHTML : le libellé vient d'un catalogue i18n, et
  //   un catalogue n'est pas un endroit d'où l'on fait entrer du HTML dans la page.
  function direReglages() {
    const el = q("pilote-reglages");
    if (!el) return;
    el.textContent = "";
    el.appendChild(document.createTextNode(T("pilote.aussi_reglages",
      "Ce pilote s'installe sur la machine elle-même, pas dans ce conteneur — et sans attendre " +
      "ce conteneur : ") + " "));
    const a = document.createElement("a");
    a.href = "/settings#deploiement/nodes";
    a.textContent = T("pilote.lien_reglages",
      "Réglages → Nœuds et matériel → Déploiement → Nœuds");
    el.appendChild(a);
  }

  // État du pilote INVÉRIFIABLE (relais en erreur : agent du nœud antérieur à 0.28.0, nœud muet).
  // ★ On ne se tait PAS, et on n'affirme pas non plus. Le bandeau rouge disait « le pilote
  //   manque » : le laisser affirmerait une absence qu'on ne mesure plus. Mais le MASQUER, ce
  //   qu'il faisait jusqu'au 31/10, laissait la seule page où l'on vient chercher avec un mot
  //   sur la version de l'agent, au fond d'un repli fermé — c'est exactement ce que Cyril a vu
  //   le 30/10 : « ça se mettait en erreur et on comprenait pas ». On dit donc l'INCONNU, en
  //   tête de page, en orange (ni une absence constatée, ni rien), et on renvoie vers Réglages.
  function dessinerPiloteInconnu() {
    const bloc = q("bloc-pilote");
    if (!bloc) return;
    bloc.classList.remove("alert-error");
    bloc.classList.add("alert-warning");
    bloc.hidden = false;
    const t = q("pilote-titre");
    if (t) t.textContent = T("pilote.titre_inconnu",
      "Impossible de vérifier le pilote Blackmagic sur cette machine");
    const quoi = q("pilote-quoi");
    if (quoi) quoi.textContent = T("pilote.inconnu_consequence",
      "L'agent de ce nœud est trop ancien pour répondre (il faut 0.28.0 ou plus), donc l'état du " +
      "pilote Desktop Video n'est pas lisible d'ici. S'il manque, ce conteneur ne peut pas " +
      "démarrer du tout : le noyau n'expose alors aucun périphérique dans /dev/blackmagic et la " +
      "carte reste invisible, même présente sur le bus PCI.");
    // Tout ce qui AGIT est retiré : on ne peut pas parler à l'agent, donc proposer un dépôt ou
    // une installation d'ici serait offrir un bouton qui échoue. Et « où trouver l'archive »
    // supposerait qu'elle manque, ce qu'on ne sait pas.
    [["pilote-plan", 1], ["bloc-depot-haut", 1], ["pilote-installer", 1],
     ["pilote-journal", 1], ["pilote-ou", 1]].forEach(([nom]) => {
      const n = q(nom); if (n) n.hidden = true;
    });
    const suite = q("pilote-suite");
    if (suite) suite.textContent = "";
    direReglages();
  }

  function dessinerPilote(d, id) {
    const bloc = q("bloc-pilote");
    if (!bloc) return;
    const st = (d && d.status) || "";
    const f = (d && d.facts) || null;
    const paquet = !!(f && f.package);
    const modules = f ? Number(f.modules || 0) : 0;
    // Pilote posé mais modules non chargés : le paquet est là, le noyau n'a rien. C'est l'état
    // d'APRÈS l'installation, et il est tout aussi bloquant — il vaut un redémarrage.
    const attendRedemarrage = paquet && modules === 0;
    const bloquant = PILOTE_ABSENT.indexOf(st) >= 0 || attendRedemarrage;
    // Retenu pour l'autre bandeau : un conteneur arrêté sur un nœud sans pilote n'est pas un
    // mystère, et le dire épargne la recherche d'une panne dans le châssis.
    piloteManque = bloquant;
    // La couleur est REPOSÉE à chaque passage : `dessinerPiloteInconnu` la fait passer en orange,
    // et un bandeau resté orange sur une absence CONSTATÉE sous-dirait ce qu'on sait.
    bloc.classList.remove("alert-warning");
    bloc.classList.add("alert-error");
    bloc.hidden = !bloquant;
    if (!bloquant) return;

    const t = q("pilote-titre");
    if (t) t.textContent = attendRedemarrage
      ? T("pilote.titre_redemarrage",
          "Pilote Desktop Video installé, modules du noyau pas encore chargés")
      : (st === "no_tool"
          ? T("pilote.titre_incomplet", "Installation du pilote Desktop Video incomplète")
          : T("pilote.titre_absent", "Pilote Desktop Video absent sur ce nœud"));
    const quoi = q("pilote-quoi");
    if (quoi) quoi.textContent = T("pilote.consequence",
      "Aucune carte ne peut être ouverte tant qu'il manque : les deux listes de cette page " +
      "resteront vides et le conteneur s'arrête quelques secondes après sa création, même si " +
      "les cartes sont bien présentes sur le bus PCI.");
    const pl = q("pilote-plan");
    const phrase = phraseMicro(d && d.plan);
    if (pl) { pl.textContent = phrase; pl.hidden = !phrase; }

    const bd = q("bloc-depot-haut"), dep = q("depot-haut");
    if (bd) bd.hidden = !(d && d.wants_upload) || firmwareEnCours;
    if (dep) {
      const lb = q("lbl-depot-haut");
      if (lb) lb.textContent = TC("settings.hostdrv.blackmagic.upload",
                                  "Déposer l'archive Blackmagic");
      dep.onchange = () => televerser(id, dep);
    }
    const ou = q("pilote-ou");
    if (ou) ou.textContent = T("pilote.ou_trouver",
      "Archive « Desktop Video » pour Linux, à prendre sur blackmagicdesign.com → Support → " +
      "Capture and Playback (elle n'est pas téléchargeable automatiquement : leur site exige " +
      "d'accepter un contrat). Fichier attendu : " +
      (d && d.upload_hint ? d.upload_hint : "Blackmagic_Desktop_Video_Linux_<version>.tar.gz") +
      ". Version en service sur ce parc au 2026-10-30 : 16.4a1 (dell-1) ; plus récent convient.");

    direReglages();
    const r0 = q("pilote-reglages"); if (r0) r0.hidden = false;
    const ou0 = q("pilote-ou"); if (ou0) ou0.hidden = false;

    const b = q("pilote-installer");
    if (b) {
      b.hidden = !(d && d.can_run) || firmwareEnCours;
      b.textContent = TC("settings.hostdrv.blackmagic.status.install", "Installer le pilote");
      b.onclick = () => lancerMicrologiciel(id, st);
    }

    const suite = q("pilote-suite");
    if (suite) {
      suite.textContent = "";
      const defauts = {
        depot: "Déposer l'archive Blackmagic ci-dessus.",
        install: "Installer le pilote (bouton ci-dessus).",
        reboot: "Redémarrer le nœud — les modules du noyau ne se chargent qu'au démarrage " +
                "suivant (Réglages → Nœuds → « Redémarrer le nœud »).",
        // ★ PLUS DE GESTE MANUEL ICI depuis que le cœur porte l'attente du pilote
        //   (`app/host_driver_gate.py`, commit 897625cf) : un conteneur créé sans pilote est
        //   enregistré SANS être lancé, et il démarre de lui-même au redémarrage du nœud, même
        //   avec l'auto-récupération à zéro. Annoncer « relancez-le » serait maintenant
        //   envoyer faire un geste inutile — et pire, laisser croire que personne ne s'en
        //   occupe si on l'oublie.
        relance: "Rien de plus à faire : le conteneur démarrera de lui-même au redémarrage, "
                 + "une fois le pilote en place."
      };
      etapesPilote(st, f).forEach((e) => {
        const li = document.createElement("li");
        li.textContent = T("pilote.etape." + e.cle, defauts[e.cle]);
        if (e.faite) li.className = "dl-faite";
        else if (e.ici) li.className = "dl-ici";
        suite.appendChild(li);
      });
    }
  }

  async function chargerMicrologiciel() {
    const bloc = q("bloc-firmware"), enveloppe = q("bloc-installation");
    if (!bloc || !editable()) return;                 // mode public : aucun appel hors proxy
    const id = await monNoeudId();
    if (!id) return;
    let d = null;
    try {
      const r = await fetch("/api/nodes/" + id + "/host-drivers/blackmagic/state",
                            { credentials: "same-origin" });
      if (!r.ok) {
        // ★ ON NE SE TAIT PAS (20/10). Le relais répond 502 tant que l'agent du nœud est
        //   antérieur à 0.28.0 — c'est le cas de toute la flotte aujourd'hui. Un bloc qui
        //   disparaît alors donne exactement la même impression qu'une fonction absente : on
        //   la cherche et on ne la trouve pas. On DIT pourquoi, puisqu'on le sait.
        bloc.hidden = false;
        if (enveloppe) enveloppe.hidden = false;
        const n0 = q("firmware-etat");
        if (n0) n0.textContent = T("firmware.indisponible",
          "Le pilote et le micrologiciel se gèrent par l'agent de la machine, qui est ici trop " +
          "ancien pour cette fonction (il faut 0.28.0 ou plus). Mettez l'agent du nœud à jour " +
          "pour voir l'état des cartes et pouvoir agir.");
        const b0 = q("firmware-maj"); if (b0) b0.hidden = true;
        const bd0 = q("bloc-depot"); if (bd0) bd0.hidden = true;
        // On ne SAIT plus si le pilote est là. Le bandeau n'est donc ni rouge (ce serait
        // affirmer une absence qu'on ne mesure plus) ni absent (c'était le défaut jusqu'au
        // 31/10, et il laissait la page muette sur le seul sujet qui bloquait) : il dit
        // l'inconnu, et il renvoie vers Réglages, où l'installation ne dépend d'aucun conteneur.
        dessinerPiloteInconnu();
        const h0 = q("firmware-cartes"); if (h0) h0.textContent = "";
        return;
      }
      d = await r.json();
    } catch (e) { return; }

    // ★ LE BLOC EST TOUJOURS LÀ — correction du 20/10, signalée aussitôt. Je l'avais masqué
    //   quand tout allait bien, au nom du bruit : « à jour » affiché en permanence n'apprend
    //   rien. Mais un bloc invisible est une capacité MORTE — on est venu chercher la gestion
    //   du pilote et on n'a rien trouvé. Le repli « Installation » suffit à le tenir hors du
    //   chemin : il est fermé par défaut, donc il ne fait pas de bruit, et il est TROUVABLE.
    // ★ CE QUI RESTE CONDITIONNEL, c'est l'OUVERTURE du repli : on ne déplie de force que
    //   lorsqu'il y a quelque chose à faire. Déplier pour annoncer « rien à faire » rendrait
    //   le dépliage lui-même insignifiant, et on cesserait de le remarquer le jour où il
    //   signale un vrai problème.
    const st = d.status || "";
    // Le bandeau bloquant d'abord : il vit en TÊTE de page, hors du repli, et c'est le premier
    // état qu'on doit lire en arrivant ici.
    dessinerPilote(d, id);
    const interessant = (st === "update" || st === "install" || st === "install_needs_file"
                         || st === "missing_prereqs" || st === "pending_shutdown"
                         || st === "unreadable" || st === "no_tool");
    bloc.hidden = false;
    if (enveloppe) enveloppe.hidden = false;
    if (enveloppe && interessant) {
      enveloppe.hidden = false;
      const titre = q("lbl-installation");
      // Ce repli-ci ne parle plus de profils : il porte le PILOTE et le MICROLOGICIEL, qui sont
      // des affaires de machine, pas de carte. Le profil, lui, est sous le ruban de sa carte.
      if (titre) titre.textContent = TC("settings.hostdrv.blackmagic.open",
                                        "Pilote et micrologiciel");
    }
    // La version du pilote vient des « rangées » que le pilote hôte publie.
    const rPaquet = (d.rows || []).find((x) => x.key === "package");
    const vue = rPaquet && rPaquet.value && rPaquet.value !== "-" ? String(rPaquet.value) : "";
    if (vue !== versionPilote) { versionPilote = vue; dessiner(); }
    const lbl = q("lbl-firmware");
    // ⚠ Le titre du catalogue central dit « Micrologiciel des cartes DeckLink du nœud ». Quand
    //   c'est le PILOTE qui manque, ce titre est faux : il envoie chercher une mise à jour de
    //   flash alors qu'aucune carte n'est même énumérée. On ne corrige pas la clé centrale —
    //   elle sert aussi la fiche du nœud, où elle est juste — on choisit l'autre titre ici.
    if (lbl) lbl.textContent = (PILOTE_ABSENT.indexOf(st) >= 0)
      ? T("pilote.titre_bloc", "Pilote et micrologiciel")
      : TC("settings.hostdrv.blackmagic.title", "Micrologiciel des cartes");
    const hote = q("firmware-cartes");
    if (hote) {
      hote.textContent = "";
      ((d.facts && d.facts.cards) || []).forEach((c) => {
        const l = document.createElement("div");
        l.className = "dl-profil-ligne";
        const n = document.createElement("span");
        n.textContent = c.model + " (" + c.id + ")";
        const e = document.createElement("span");
        e.className = "dl-fixe";
        e.textContent = TC("settings.hostdrv.blackmagic.status."
                           + (c.state === "ok" ? "ok" : "update"),
                           c.state === "ok" ? "à jour" : "à mettre à jour");
        l.appendChild(n); l.appendChild(e);
        hote.appendChild(l);
      });
    }
    // ★ LES INFORMATIONS DE LA MACHINE NE SONT PAS DES CARTES. Je les avais empilées dans la
    //   même liste : le compte de cartes devenait faux, et un lecteur pressé lisait « paquet »
    //   comme une troisième carte. Un conteneur distinct, parce que ce sont deux natures
    //   différentes — l'une par carte, l'autre par machine.
    if (hote) {
      let infos = Array.from(hote.parentNode ? hote.parentNode.children : [])
        .find((x) => x.dataset && x.dataset.dl === "firmware-infos");
      if (!infos) {
        infos = document.createElement("div");
        infos.className = "dl-liste";
        infos.dataset.dl = "firmware-infos";
        if (hote.parentNode) hote.parentNode.insertBefore(infos, hote.nextSibling);
      }
      infos.textContent = "";
      (d.rows || []).filter((x) => x.key !== "card").forEach((x) => {
        const l = document.createElement("div");
        l.className = "dl-profil-ligne";
        const n0 = document.createElement("span");
        n0.textContent = TC("settings.hostdrv.blackmagic.row." + x.key, x.key);
        const v0 = document.createElement("span");
        v0.className = "dl-fixe";
        v0.textContent = x.value;
        l.appendChild(n0); l.appendChild(v0);
        infos.appendChild(l);
      });
    }
    const note = q("firmware-etat");
    if (note) note.textContent = phraseMicro(d.plan);
    const b = q("firmware-maj");
    if (b) {
      b.hidden = !d.can_run || firmwareEnCours;
      // Le même bouton sert aux deux travaux, et il doit DIRE lequel : « mettre à jour le
      // micrologiciel » sur un nœud où le pilote manque serait un contresens.
      b.textContent = (st === "install")
        ? TC("settings.hostdrv.blackmagic.status.install", "Installer le pilote")
        : TC("settings.hostdrv.blackmagic.run", "Mettre à jour le micrologiciel");
      b.onclick = () => lancerMicrologiciel(id, st);
    }
    const bd = q("bloc-depot"), dep = q("depot");
    if (bd) bd.hidden = !d.wants_upload || firmwareEnCours;
    if (dep) {
      const lb = q("lbl-depot");
      if (lb) lb.textContent = TC("settings.hostdrv.blackmagic.upload",
                                  "Déposer l'archive Blackmagic")
                               + (d.upload_hint ? " — " + d.upload_hint : "");
      dep.onchange = () => televerser(id, dep);
    }
  }

  async function televerser(id, champ) {
    // Le corps est le fichier BRUT : l'agent le relaie par morceaux et ne le tient jamais en
    // mémoire. Une archive Desktop Video pèse plusieurs dizaines de Mo.
    const f = champ && champ.files && champ.files[0];
    if (!f) return;
    jVider();
    jLog("… " + f.name + " (" + f.size + " o)\n");
    try {
      const r = await fetch("/api/nodes/" + id + "/host-drivers/blackmagic/upload?name="
                            + encodeURIComponent(f.name),
                            { method: "POST", credentials: "same-origin", body: f });
      if (!r.ok) throw new Error("HTTP " + r.status);
      jLog(TC("settings.hostdrv.blackmagic.status.install", "Pilote à installer") + "\n");
    } catch (e) {
      jLog((e && e.message ? e.message : String(e)) + "\n");
      return;
    }
    await chargerMicrologiciel();
  }

  async function lancerMicrologiciel(id, st) {
    // ⚠ LE MÊME BOUTON FAIT DEUX TRAVAUX, ET L'AVERTISSEMENT DOIT SUIVRE. Il annonçait
    //   toujours un ARRÊT COMPLET : sur un nœud où le pilote manque, c'est un contresens —
    //   l'installation demande un REDÉMARRAGE (les modules se chargent au boot), pas une
    //   coupure d'alimentation, qui est le prix d'une écriture de micrologiciel.
    const pose = (st === "install" || st === "upgrade");
    const avert = pose
      ? (phraseMicro({ code: "driver_install_from_file", params: {} })
         || TC("settings.hostdrv.blackmagic.status.install", "Installer le pilote"))
        + "\n\n" + TC("settings.hostdrv.pending.reboot",
                       "Redémarrage requis pour que le changement prenne effet.")
      : TC("settings.hostdrv.blackmagic.plan.firmware_outdated", "")
        + "\n\n" + TC("settings.hostdrv.pending.shutdown",
                       "Arrêt COMPLET requis pour que le changement prenne effet.");
    if (!window.confirm(avert)) return;
    firmwareEnCours = true;
    const b = q("firmware-maj"); if (b) b.hidden = true;
    const bh = q("pilote-installer"); if (bh) bh.hidden = true;
    jVider();
    try {
      const r = await fetch("/api/nodes/" + id + "/host-drivers/blackmagic/run",
                            { method: "POST", credentials: "same-origin",
                              headers: { "Content-Type": "application/json" }, body: "{}" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      await suivreJob(id, 0);
    } catch (e) {
      jLog("\n" + (e && e.message ? e.message : e));
    }
    firmwareEnCours = false;
  }

  async function suivreJob(id, offset) {
    // Le job est LONG (plusieurs minutes d'écriture de flash). On relit le journal par
    // décalage, comme le panneau des réglages, et on s'arrête quand l'agent dit que c'est fini.
    for (let i = 0; i < 600 && firmwareEnCours; i++) {
      let d = null;
      try {
        const r = await fetch("/api/nodes/" + id + "/host-drivers/blackmagic/job?offset=" + offset,
                              { credentials: "same-origin" });
        d = r.ok ? await r.json() : null;
      } catch (e) { d = null; }
      if (d) {
        if (d.log) { jLog(d.log); offset = d.offset || offset; }
        if (d.state && d.state !== "running") {
          await chargerMicrologiciel();
          return;
        }
      }
      await new Promise((res) => setTimeout(res, 2000));
    }
  }

  function mount(el, vmid, ctx) {
    EL = el; VMID = vmid;
    BASE = (ctx && ctx.base) || null;
    if (editable()) {
      q("role").addEventListener("change", majChamps);
      q("key-pid").addEventListener("change", majChamps);
      q("appliquer").addEventListener("click", ajouterAuLot);
      q("annuler").addEventListener("click", fermerEditeur);
      q("lot-appliquer").addEventListener("click", appliquerLot);
      q("lot-abandonner").addEventListener("click", abandonnerLot);
      q("appliquer").textContent = T("bouton.ajouter", "Ajouter aux changements");
      q("lot-appliquer").textContent = T("bouton.appliquer_lot", "Appliquer (redémarre la carte)");
      q("lot-abandonner").textContent = T("bouton.abandonner", "Abandonner");
      q("annuler").textContent = T("bouton.annuler", "Annuler");
    }
    dessinerLegende();
    rafraichir();
    minuteur = setInterval(rafraichir, PERIODE_MS);
    // ★ PAS DANS LA BOUCLE DES 2 SECONDES. Lire le micrologiciel interroge l'AGENT DU NŒUD, pas
    //   notre conteneur : à la cadence du bandeau, on frapperait l'agent 30 fois par minute pour
    //   une information qui ne change qu'au branchement d'une carte ou après une mise à jour.
    //   Une fois à l'ouverture, puis toutes les deux minutes.
    chargerMicrologiciel();
    minuteurMicro = setInterval(chargerMicrologiciel, 120000);
  }

  function unmount() {
    if (minuteur) clearInterval(minuteur);
    // Le second minuteur doit mourir avec le premier : un intervalle oublié continue d'interroger
    // l'agent du nœud après que la page a changé — invisible, et ça ne s'arrête jamais.
    if (minuteurMicro) clearInterval(minuteurMicro);
    minuteurMicro = 0;
    firmwareEnCours = false;
    injoignableDepuis = 0; etatConteneurLu = 0; piloteManque = false;
    minuteur = 0; EL = null; inventaire = null; etat = null;
  }

  // Crochet de BANC, pas d'interface : le bloc micrologiciel parle à l'agent du nœud, donc le
  // banc doit pouvoir le déclencher seul — et surtout pouvoir rejouer le cas « page publique »,
  // où AUCUN de ces appels ne doit sortir. Sans ce crochet, cette branche n'était testée par
  // rien, et c'est exactement ainsi qu'on livre une page publique qui boucle en 401.
  async function _micro(opt) {
    const sauve = BASE, sauveEl = EL, sauveVmid = VMID;
    if (opt && opt.public) BASE = "/p/jeton-de-banc";
    if (opt && opt.vmid) VMID = opt.vmid;
    // Le banc peut fournir son propre DOM : c'est ce qui permet d'exercer le CHEMIN RÉEL
    // (lecture de l'état du nœud, puis rendu) au lieu de la seule fonction de dessin — et donc
    // d'attraper un bandeau qu'on aurait oublié d'appeler depuis là.
    if (opt && opt.el) EL = opt.el;
    noeudId = null;
    try { await chargerMicrologiciel(); }
    finally { BASE = sauve; EL = sauveEl; VMID = sauveVmid; }
  }

  // Crochet de BANC pour le bandeau « pilote absent ». Même raison que `_micro` : cette
  // branche ne s'ouvre que sur un nœud SANS pilote, c'est-à-dire exactement la machine qu'on
  // n'a pas sous la main quand on écrit le code — et c'est l'état le plus coûteux à rater,
  // puisqu'il est le seul où la page n'a rien d'autre à montrer.
  function _pilote(el, d) {
    const sauve = EL;
    EL = el;
    try { dessinerPilote(d, 1); } finally { EL = sauve; }
  }

  // Le banc peut fournir son DOM et son vmid — même raison que `_micro`. L'appel sans argument
  // reste celui de `banc_console.js`.
  async function _rafraichir(opt) {
    const sauveEl = EL, sauveVmid = VMID, sauveBase = BASE;
    if (opt && opt.el) EL = opt.el;
    if (opt && opt.vmid) VMID = opt.vmid;
    if (opt && opt.public) BASE = "/p/jeton-de-banc";
    try { await rafraichir(); }
    finally { EL = sauveEl; VMID = sauveVmid; BASE = sauveBase; }
  }

  // Un clic de banc. En service, `EL` est posé par `mount` et reste en place tant que la page
  // vit : un gestionnaire peut donc interroger le DOM au moment du clic. Le banc, lui, n'a pas
  // monté la page — d'où ce crochet, qui rend `EL` le temps du geste.
  async function _clic(el, nom, vmid) {
    const sauveEl = EL, sauveVmid = VMID;
    EL = el;
    if (vmid) VMID = vmid;
    try {
      const n = el.querySelector('[data-dl="' + nom + '"]');
      if (!n || typeof n.onclick !== "function") return false;
      await n.onclick();
      return true;
    } finally { EL = sauveEl; VMID = sauveVmid; }
  }

  // Crochet de BANC pour les changements en LOT (07/11) : la page n'est pas montée, le banc pose
  // son DOM, son vmid et la configuration en base, puis joue les VRAIS gestes — choisir une prise,
  // « Ajouter aux changements », relire, « Appliquer ». La garde vérifie qu'un lot part en UN envoi.
  function _bancLot(el, vmid, cfg) {
    EL = el; VMID = vmid; config = cfg || null;
    lot = null; lotOrigine = null; choisi = null; enAttente = null;
    aConfirmer = false; plafondAccepte = false; envoiEnCours = false;
    return {
      choisir: (pid) => { choisi = pid; },
      ajouter: ajouterAuLot,
      appliquer: appliquerLot,
      abandonner: abandonnerLot,
      rafraichir: rafraichir,
      majLot: majLot,
      etat: () => ({ lot: lot, lotOrigine: lotOrigine, config: config, choisi: choisi }),
    };
  }

  return { mount: mount, unmount: unmount, _micro: _micro, _bancLot: _bancLot, _redessiner: dessiner,
           _rafraichir: _rafraichir, _pilote: _pilote, _etapesPilote: etapesPilote,
           _clic: _clic, _legende: (el) => {
             const sauve = EL; EL = el;
             try { dessinerLegende(); } finally { EL = sauve; }
           } };
})();
