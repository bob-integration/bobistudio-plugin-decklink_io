// SPDX-License-Identifier: GPL-3.0-or-later
// Banc de la CONSOLE du plugin decklink_io — monte `control.js` dans un DOM simulé, le pilote,
// et vérifie ce qu'il POSTE. Sans carte : il travaille sur un relevé de sonde enregistré.
//
//   node tools/console/banc_console.js        (depuis la racine du plugin)
//
// ★ POURQUOI UN BANC PLUTÔT QU'UNE RELECTURE. `node --check` ne voit que la syntaxe, et la
//   console a maintenant de la logique : rôles, appariement Fill & Key, pré-charge, listes
//   dérivées. Un défaut y est muet — un champ oublié dans le corps posté n'affiche aucune
//   erreur, il efface simplement un réglage. Deux l'ont déjà été dans ce plugin.
const fs = require("fs"), path = require("path");
const RACINE = path.resolve(__dirname, "..", "..");
// DL_INV choisit le relevé : Duo 2 réel par défaut, Quad 2 synthétique pour éprouver 8 prises.
const FICHIER = process.env.DL_INV || "inventaire_duo2.json";
// Le relevé dit lui-même si la carte sait changer de profil : deux profils offerts au moins.
// Aucun contrôle de ce banc ne doit se conditionner au NOM du fichier — cinq fois que ça rate.
const inv = JSON.parse(fs.readFileSync(path.join(__dirname, FICHIER), "utf8"));
// Vrai si la carte offre de quoi CHANGER de profil (la console n'affiche le bloc qu'à partir de
// deux). `profils: null` = pas de gestionnaire de profil, cas d'une carte mono-profil.
const multiProfil = ((inv.devices || [])[0] || {}).profils instanceof Array
                 && (inv.devices[0].profils || []).length >= 2;
const N_SUB = (inv.devices || []).length;
console.log("relevé : " + FICHIER + " (" + N_SUB + " sous-périphériques)");

// ⚠ UNE ENTRÉE VIVANTE EST NÉCESSAIRE : sans mesure dans /state, le bandeau tombe sur la
//   branche « format détecté » et la branche VIVANTE n'est jamais exercée. C'est elle qui porte
//   la cadence mesurée, donc la seule capable de démentir le format annoncé par la carte.
// Une SORTIE vivante est nécessaire elle aussi : sans elle, la branche qui affiche le format
// ÉMIS n'est jamais exercée, et c'est précisément celle que l'exploitant a demandée.
const etat = { hostname: "decklink-dell1", outputs: [], voies: [],
               inputs: [{ persistent_id: null, flow: "decklink-dell1_1", fps: 50.02,
                          width: 1920, height: 1080, mode: "actif" }] };
// ⚠ LA CONFIGURATION DE TEST SE DÉRIVE DU RELEVÉ, elle ne le présuppose pas. Elle référençait en
//   dur l'identifiant de la Duo 2 : sur un autre relevé, plus aucun connecteur ne correspondait
//   et le banc échouait sur du code SAIN. Troisième fois que ce banc se trompe lui-même — un
//   banc qui encode des hypothèses sur son décor casse au premier changement de décor.
const PID_ENTREE = inv.devices[0].persistent_id;
etat.inputs[0].persistent_id = PID_ENTREE;      // l'état vivant porte la MÊME identité
const PID_AUTRE = (inv.devices.find((d) => d.persistent_id !== PID_ENTREE
                                        && d.duplex !== "inactive") || {}).persistent_id;
// Une sortie DÉJÀ CÂBLÉE : son flux vient de la page Câbles, pas de la console. Il doit
// survivre à tout réglage fait ici — l'effacer rendrait la sortie muette sans rien dire.
const PID_SORTIE = (inv.devices.find((d) => d.persistent_id !== PID_ENTREE
                                         && d.duplex !== "inactive") || {}).persistent_id;
// Le groupement Fill & Key ne se teste que si la carte a de quoi : sur une Duo 2, assigner
// entrée + remplissage + découpe ne laisse plus AUCUNE prise libre, et les autres contrôles
// n'auraient plus de connecteur à éditer. On ne truque pas le décor pour faire passer un test.
const libres = inv.devices.filter((d) => d.duplex !== "inactive");
const PID_KEY = libres.length >= 5
  ? (libres.find((d) => ![PID_ENTREE, PID_SORTIE].includes(d.persistent_id)) || {}).persistent_id
  : null;
// `prise: 99` est VOLONTAIREMENT impossible comme rang de carte : c'est le repère de
// l'exploitant, relevé au châssis. La console ne doit JAMAIS le remplacer par le rang du
// connecteur dans l'inventaire (22/10 : huit numéros détruits ainsi, la page Câbles
// désignait les mauvaises prises).
const sortie0 = { persistent_id: PID_SORTIE, flow: "mixer_pgm", mode: "1080i50", preroll: 2,
                  prise: 99 };
if (PID_KEY) { sortie0.key_persistent_id = PID_KEY; sortie0.key_flow = "mixer_key"; }
const persiste = { params: {
  inputs:  [{ persistent_id: PID_ENTREE, flow: "decklink-dell1_1", audio: 16, anc: true }],
  outputs: [sortie0] } };
etat.voies.push({ sortie: 1, voie: "fill", persistent_id: PID_SORTIE, flow: "mixer_pgm",
                  mode: "émet", fps: 25.0, width: 1920, height: 1080 });
// `vmid` et `node` sont ce que rend réellement /api/sources : la console s'en sert pour écarter
// les flux d'une AUTRE machine, qui ne sont pas joignables (bus mémoire local).
const sources = [
  { shm: "decklink-dell1_1", label: "SDI", vmid: 1105, node: "dell-1" },
  { shm: "mixer_pgm",  label: "PGM", vmid: 900,  node: "dell-1" },
  { shm: "mixer_key",  label: "KEY", vmid: 900,  node: "dell-1" },
  { shm: "2110-io-dl360-1_1", label: "ailleurs", vmid: 700, node: "dl360-1" },
  { shm: "2110-io-dl360-1_2", label: "ailleurs", vmid: 700, node: "dl360-1" },
];
let poste = null;
let PID_DECOUPE = null;   // choisi dans la liste RÉELLE, pas imposé
let sourcesEnPanne = false;

class El {
  static reattachesInutiles = 0;
  constructor(tag) { this.tag = tag; this.children = []; this.dataset = {}; this.className = "";
    this._text = ""; this.hidden = false; this._value = ""; this.checked = false;
    this.parentNode = null; this.title = ""; this.innerHTML = ""; this._ev = {}; }
  set textContent(v) { this._text = v; this.children = []; }
  // ★ UN <select> REFUSE UNE VALEUR QUI N'EST PAS DANS SES OPTIONS — le navigateur la ramène à
  //   "". Ce faux DOM l'acceptait, et il a menti DEUX fois : il a validé un Fill & Key sur un
  //   seul connecteur (impossible), puis un champ « rempli » alors que la liste n'avait pas
  //   chargé. Un banc plus permissif que le navigateur affirme en vert des états que
  //   l'interface réelle ne peut pas produire — c'est pire que pas de banc.
  set value(v) {
    const opts = this.children.filter((c) => c.tag === "option");
    // On se fie à la PRÉSENCE d'options, pas au nom de balise : ce faux DOM crée tout en
    // <div>, donc tester `tag === "select"` ne mordait sur rien.
    if (opts.length && v !== ""
        && !opts.some((o) => String(o.value) === String(v))) { this._value = ""; return; }
    this._value = v;
  }
  get value() { return this._value; }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(" "); }
  // ★ `appendChild` DÉPLACE, il ne duplique pas — comme le vrai DOM (20/10). Ce faux DOM se
  //   contentait d'empiler : un élément réattaché apparaissait DEUX FOIS, et un contrôle qui
  //   compte les éléments voyait un doublon là où le navigateur n'en a aucun. Un banc qui se
  //   comporte autrement que le navigateur affirme en vert ce que l'interface ne fait pas —
  //   c'est déjà arrivé aujourd'hui avec `.children` traité comme un tableau.
  // `insertBefore` manquait : la console l'utilise pour poser un bloc à un endroit PRÉCIS, et
  // le banc tombait sur « n'est pas une fonction ». Un faux DOM incomplet ne se contente pas de
  // mal juger — il empêche d'exécuter. Même détachement préalable que `appendChild`.
  insertBefore(c, ref) {
    if (c.parentNode) {
      const j = c.parentNode.children.indexOf(c);
      if (j >= 0) c.parentNode.children.splice(j, 1);
    }
    c.parentNode = this;
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i >= 0) this.children.splice(i, 0, c); else this.children.push(c);
    return c;
  }
  appendChild(c) {
    // ★ ON COMPTE LES RÉ-ATTACHEMENTS INUTILES (20/10). Rattacher un élément à son parent
    //   ACTUEL est invisible dans le DOM final, mais pas pour l'exploitant : ça referme une
    //   liste déroulante ouverte et interrompt une saisie. Deux défauts de la journée venaient
    //   de là, et aucun contrôle sur l'état final ne pouvait les voir — il faut compter le
    //   GESTE, pas regarder le résultat.
    if (c.parentNode === this) El.reattachesInutiles++;
    if (c.parentNode && c.parentNode !== this) {
      const i = c.parentNode.children.indexOf(c);
      if (i >= 0) c.parentNode.children.splice(i, 1);
    } else if (c.parentNode === this) {
      const i = this.children.indexOf(c);
      if (i >= 0) this.children.splice(i, 1);   // réattacher = remettre à la fin
    }
    c.parentNode = this; this.children.push(c); return c;
  }
  // `setAttribute` et `childElementCount` manquaient, et le banc s'ARRÊTAIT dessus : la légende
  // des couleurs (0.29.44) pose un `aria-hidden` sur chaque pastille, donc plus aucun contrôle
  // posté ne s'exécutait après son arrivée. Un faux DOM incomplet n'affaiblit pas le verdict,
  // il l'empêche — même leçon qu'`insertBefore` plus haut. Les attributs `data-*` vont dans
  // `dataset`, comme dans le navigateur, pour que les deux chemins ne divergent pas.
  setAttribute(n, v) {
    if (String(n).startsWith("data-")) {
      const k = String(n).slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      this.dataset[k] = String(v); return;
    }
    (this._attrs = this._attrs || {})[n] = String(v);
  }
  getAttribute(n) {
    if (String(n).startsWith("data-")) {
      const k = String(n).slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      return k in this.dataset ? this.dataset[k] : null;
    }
    return (this._attrs && n in this._attrs) ? this._attrs[n] : null;
  }
  get childElementCount() { return this.children.length; }
  // `classList` opère sur `className`, jamais sur une liste parallèle : la console écrit les deux
  // (`el.className = "…"` ici, `classList.add` là), et deux états séparés divergeraient en
  // silence — le banc verrait une classe que le navigateur n'a pas.
  get classList() {
    const lire = () => String(this.className || "").split(/\s+/).filter(Boolean);
    const ecrire = (l) => { this.className = l.join(" "); };
    return {
      add: (...c) => { const l = lire(); c.forEach((x) => { if (!l.includes(x)) l.push(x); }); ecrire(l); },
      remove: (...c) => ecrire(lire().filter((x) => !c.includes(x))),
      contains: (c) => lire().includes(c),
      toggle: (c, f) => { const l = lire(); const a = (f === undefined) ? !l.includes(c) : !!f;
                          ecrire(a ? (l.includes(c) ? l : l.concat([c])) : l.filter((x) => x !== c)); return a; },
    };
  }
  addEventListener(t, f) { (this._ev[t] = this._ev[t] || []).push(f); }
  declencher(t) { (this._ev[t] || []).forEach((f) => f()); }
  querySelector(sel) {
    const m = /\[data-dl="(.+?)"\]/.exec(sel); if (!m) return null;
    const c = (n) => { if (n.dataset && n.dataset.dl === m[1]) return n;
      for (const k of n.children) { const r = c(k); if (r) return r; } return null; };
    return c(this);
  }
}
// `createTextNode` : un nœud de texte est un enfant comme un autre pour ce faux DOM — il porte
// son texte et rien d'autre. Sans lui, le bandeau du pilote s'arrêtait sur une TypeError.
global.document = { createElement: (t) => new El(t),
                    createTextNode: (txt) => { const n = new El("#text");
                                               n.textContent = String(txt); return n; } };
// `window.confirm` était ABSENT : tout clic sur « Appliquer » levait donc une TypeError, et le
// chemin de bascule n'a jamais pu être exercé par ce banc. On le pilote, et on garde les
// questions posées — c'est leur TEXTE qui doit nommer la prise qu'on s'apprête à couper.
let reponsesConfirm = [];
let questions = [];
global.window = { confirm: (q) => { questions.push(String(q));
                                    return reponsesConfirm.length ? reponsesConfirm.shift() : true; } };
// Scénario de bascule de profil : quand `reponseProfil` est posée, le POST /profile la rend
// au lieu d'un « ok ». Sans ça le banc ne pouvait pas éprouver le chemin de CONFIRMATION — il
// répondait toujours oui, donc le refus de la carte n'était jamais joué.
let postes = [];                 // TOUS les POST, pas seulement le dernier : c'est ce qui permet
                                 // de prouver qu'un second envoi a eu lieu — ou n'a PAS eu lieu.
let reponseProfil = null;
let appelsCoeur = [];            // URL hors proxy de plugin (agent du nœud)
// Par défaut : rien à signaler. Les contrôles qui veulent l'autre cas le posent eux-mêmes.
let etatFirmware = { status: "ok", can_run: false,
                     plan: { status: "ok", code: "all_up_to_date", params: { count: 1 } },
                     rows: [{ key: "package", value: "16.4a1" },
                            { key: "prereqs", value: "dkms yes, kernel headers yes" }],
                     facts: { cards: [{ model: "DeckLink Duo 2", id: "Gf0d8aa00", state: "ok" }] } };
global.fetch = async (u, o) => {
  if (o && o.method === "POST") {
    const corps = JSON.parse(o.body);
    postes.push({ url: u, corps: corps });
    poste = corps;
    if (u.indexOf("/profile") >= 0 && reponseProfil) {
      const r = reponseProfil.shift() || { ok: true };
      return { ok: true, json: async () => r };
    }
    return { ok: true, json: async () => ({ ok: true }) };
  }
  if (u.indexOf("/inventory") >= 0) return { ok: true, json: async () => inv };
  // ── Pilote hôte « blackmagic » (micrologiciel). Ces appels sortent du proxy de plugin : ils
  //    visent l'AGENT DU NŒUD. Le banc les enregistre pour prouver deux choses opposées — qu'ils
  //    ont lieu en mode éditable, et qu'ils n'ont JAMAIS lieu derrière un jeton public.
  if (u.indexOf("/api/containers") >= 0 && u.indexOf("/plugin") < 0) {
    appelsCoeur.push(u);
    return { ok: true, json: async () => ([{ vmid: 1105, node_id: 35 }]) };
  }
  if (u.indexOf("/host-drivers/blackmagic/state") >= 0) {
    appelsCoeur.push(u);
    return { ok: true, json: async () => etatFirmware };
  }
  if (u.indexOf("/api/sources") >= 0) {
    // Aucun producteur local : cas RÉEL (la machine n'héberge aucun flux vidéo), et c'est lui
    // qui vide le champ. Un échec RÉSEAU, lui, conserve la dernière liste connue — ce qui est
    // le bon comportement et ne vide donc rien.
    return { ok: true, json: async () => (sourcesEnPanne ? [] : sources) };
  }
  if (u.indexOf("/plugin_config") >= 0) return { ok: true, json: async () => persiste };
  return { ok: true, json: async () => etat };
};
global.setInterval = () => 0; global.clearInterval = () => {};

const racine = new El("div");
// ⚠ LES PRISES NE SONT PLUS DES ENFANTS DIRECTS DU BANDEAU (20/10) : elles vivent dans un
// ruban PAR CARTE. On descend donc l'arbre au lieu de lire un seul niveau. C'est le banc qui a
// signalé le changement de structure, en TOMBANT NET — pas en vérifiant tranquillement zéro
// prise, ce qu'un `.filter()` sur un niveau vide aurait fait sans un mot.
// ── CONTRÔLE STATIQUE : pas de méthode de TABLEAU sur une collection du DOM ────────────────
// `element.children` est une HTMLCollection : elle n'a ni `.find`, ni `.filter`, ni `.map`.
// Le faux DOM de ce banc, lui, utilise de vrais tableaux — il a donc affiché cinq relevés au
// VERT sur du code qui tombait net dans un navigateur (« .find is not a function »), et c'est
// l'exploitant qui l'a découvert, console entièrement morte.
//
// Rendre le faux DOM aussi strict serait l'idéal, mais le banc lui-même parcourt ces listes
// partout : on gagne la même protection, pour bien moins de risque, en REFUSANT le motif à la
// lecture. `Array.from(x.children).find(...)` reste permis — c'est la forme correcte.
// ── CONTRAT DU MANIFESTE POUR CE QUE LA CONSOLE ÉCRIT ────────────────────────────────────
// Toute clé que la console POSTE doit être déclarée dans `config_schema`, sinon le serveur la
// REFUSE : « clés hors config_schema ». Les dispositions enregistrées sont parties ainsi en
// erreur dès le premier essai de l'exploitant, et rien dans la console ne pouvait l'anticiper.
// Et elles doivent porter `redeploiement: false` : le conteneur ne les lit JAMAIS — elles sont
// lues par la console, dans deploy_config. Redéployer pour enregistrer un nom couperait les
// entrées et les sorties de la carte pour une ligne de texte.
function verifierManifeste() {
  const m = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "..", "plugin.json"), "utf8"));
  const champs = m.config_schema || [];
  const src = fs.readFileSync(path.join(__dirname, "..", "..", "control.js"), "utf8");
  const postees = new Set();
  const re = /posterConfig\(\s*\{\s*([A-Za-z_][\w]*)\s*:/g;
  let x;
  while ((x = re.exec(src)) !== null) postees.add(x[1]);
  const declarees = new Set(champs.map((f) => f.key));
  const manquantes = Array.from(postees).filter((k) => !declarees.has(k));
  // ★ ET CE QUE LA CONSOLE ÉCRIT, ELLE DOIT SAVOIR LE RELIRE. Les dispositions partaient bien
  //   au serveur mais étaient JETÉES à la relecture : la liste restait vide, et l'exploitant
  //   voyait « enregistrée » puis plus rien. Un aller sans retour n'est pas un enregistrement.
  {
    const lecteur = src.slice(src.indexOf("async function lireConfig"),
                              src.indexOf("async function rafraichir"));
    const perdues = Array.from(postees).filter((k) => lecteur.indexOf(k) < 0);
    verifier("ce que la console enregistre, elle sait le relire",
             perdues.length === 0, perdues.join(", "));
  }
  verifier("toute clé postée par la console est déclarée au manifeste",
           manquantes.length === 0, manquantes.join(", "));
  const dispo = champs.find((f) => f.key === "layouts");
  verifier("les dispositions n'entraînent PAS de redéploiement",
           !!dispo && dispo.redeploiement === false,
           dispo ? String(dispo.redeploiement) : "clé absente");
}

// ── GABARIT ET SCRIPT NE DOIVENT PAS DÉRIVER ─────────────────────────────────────────────
// ★★ Tout repère que `control.js` va CHERCHER doit exister dans `control.html`. Sinon `q()`
//    rend null et la première propriété posée dessus lève — ce qui ne tue pas une ligne mais
//    TOUTE la console, écran blanc, exactement comme le 20/10 (`children.find` n'existe pas
//    sur une HTMLCollection). Le banc le disait déjà, mais en PLANTANT sur le décor : un
//    plantage du harnais ne nomme pas la cause, et il arrête tous les contrôles suivants.
function verifierGabaritEtScript() {
  const src = fs.readFileSync(path.join(__dirname, "..", "..", "control.js"), "utf8");
  const html = fs.readFileSync(path.join(__dirname, "..", "..", "control.html"), "utf8");
  const dansGabarit = new Set((html.match(/data-dl="([^"]+)"/g) || []).map((m) => m.slice(9, -1)));
  // Les repères créés par le script lui-même au moment du rendu : ils n'ont pas à être dans
  // le gabarit. La liste est COURTE et explicite — l'allonger doit se voir en revue.
  const posesParLeScript = new Set(["edit-titre", "liste", "erreur", "bloc", "zone"]);
  const manquants = [];
  const re = /\bq\("([^"]+)"\)/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    const nom = m[1];
    if (!dansGabarit.has(nom) && !posesParLeScript.has(nom) && !manquants.includes(nom))
      manquants.push(nom);
  }
  verifier("tout repère cherché par le script existe dans le gabarit",
           manquants.length === 0, manquants.join(", "));
}

// ── CONTRÔLES DE LA CHARTE ────────────────────────────────────────────────────────────────
// Tout champ ou bouton créé par la console doit porter la classe de l'interface — `ctl-input`
// pour un <select>/<input>, `btn` pour un <button> — et jamais son propre style. Sinon la
// console dérive du reste du produit à chaque évolution de la charte.
// Signalé sur capture d'écran le 20/10 : la barre des dispositions alignait trois contrôles de
// trois hauteurs différentes. Les classes y étaient ; c'est la règle de TAILLE (44 px, cible
// tactile) qui ne s'appliquait qu'à `.dl-champ` et `.dl-actions` — donc tout bloc nouveau
// naissait hors charte en silence.
function verifierControlesCharte() {
  const src = fs.readFileSync(path.join(__dirname, "..", "..", "control.js"), "utf8");
  const css = fs.readFileSync(path.join(__dirname, "..", "..", "control.css"), "utf8");
  const sansClasse = [];
  const re = /createElement\("(select|input|button)"\)/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    // On regarde les 400 caractères qui suivent : la classe est posée juste après la création.
    const suite = src.slice(m.index, m.index + 400);
    const attendue = m[1] === "button" ? "btn" : "ctl-input";
    // ⚠ UNE EXCEPTION, ET UNE SEULE : un champ MASQUÉ n'est jamais affiché, donc il n'a pas à
    //   porter la charte. C'est le cas du sélecteur de fichier, dont le rendu natif est laid
    //   et large — on le masque et c'est un bouton de la charte qui le déclenche. L'exception
    //   est écrite ici plutôt que de relâcher la règle : elle doit rester VÉRIFIABLE.
    if (/\.hidden\s*=\s*true/.test(suite)) continue;
    if (suite.indexOf(attendue) < 0) {
      sansClasse.push("ligne " + src.slice(0, m.index).split("\n").length + " : <" + m[1] + ">");
    }
  }
  verifier("tout champ créé porte la classe de la charte", sansClasse.length === 0,
           sansClasse.join(" | "));
  // Chaque conteneur de contrôles doit porter la cible tactile ; l'iPad est la référence.
  const blocs = Array.from(new Set((src.match(/className = "dl-[a-z-]+"/g) || [])
    .map((x) => x.slice('className = "'.length, -1))))
    .filter((c) => ["dl-champ", "dl-actions", "dl-dispositions"].indexOf(c) >= 0);
  // ⚠ ON DÉCOUPE EN RÈGLES, on ne cherche pas un motif à cheval. Ma première version exigeait
  //   `min-height` AVANT la première accolade — c'est-à-dire dans le SÉLECTEUR — donc elle
  //   échouait sur du CSS parfaitement correct. Un contrôle qui ne peut que tomber ne vaut pas
  //   mieux qu'un contrôle qui ne peut que passer.
  const regles = css.split("}").map((r) => {
    const i = r.indexOf("{");
    return i < 0 ? null : { sel: r.slice(0, i), corps: r.slice(i + 1) };
  }).filter(Boolean);
  const sansCible = blocs.filter((c) => !regles.some(
    (r) => r.sel.indexOf("." + c) >= 0 && /min-height:\s*44px/.test(r.corps)));
  verifier("chaque barre de contrôles porte la cible tactile de 44 px",
           sansCible.length === 0, sansCible.join(", "));
}

function verifierCollectionsDom() {
  const src = fs.readFileSync(path.join(__dirname, "..", "..", "control.js"), "utf8");
  const motif = /(?:\.children\s*\|\|\s*\[\]\s*\)|\.children)\s*\.\s*(find|filter|map|some|every|reduce|forEach|slice|sort|concat|indexOf|includes)\b/g;
  // ⚠ LA PREMIÈRE VERSION DE CETTE EXCLUSION ÉTAIT AUTO-ANNULANTE : elle réécrivait la fin du
  //   texte précédent en « Array.from( » AVANT de tester s'il s'y terminait, donc le test
  //   réussissait TOUJOURS et toutes les fautes étaient ignorées. Le contrôle s'affichait en
  //   vert en ne regardant rien — et il est resté vert quand j'ai remis la faute qui avait tué
  //   la console. Un contrôle qu'on ne mute pas est une décoration.
  const fautes = [];
  let m;
  while ((m = motif.exec(src)) !== null) {
    const avant = src.slice(Math.max(0, m.index - 40), m.index);
    if (avant.indexOf("Array.from") >= 0) continue;   // forme correcte, autorisée
    const ligne = src.slice(0, m.index).split("\n").length;
    fautes.push("ligne " + ligne + " : .children." + m[1]);
  }
  verifier("aucune méthode de tableau appelée sur une collection du DOM",
           fautes.length === 0, fautes.join(" | "));
}

function prisesDe(noeud) {
  const acc = [];
  (function descendre(n) {
    (n.children || []).forEach((c) => {
      if (c.dataset && c.dataset.role) acc.push(c);
      descendre(c);
    });
  })(noeud);
  return acc;
}

// ★★ LA LISTE DES ÉLÉMENTS SE DÉRIVE DU GABARIT, elle ne se recopie pas (21/10). Elle était
//    écrite à la main ici, donc elle DÉRIVAIT : au premier élément ajouté à `control.html`, le
//    banc explosait sur un `null` au lieu de tester quoi que ce soit — et un banc qui plante
//    sur le décor ne dit plus rien sur la console. Pire : son faux DOM ne décrivait plus le
//    vrai, donc ce qu'il validait n'était plus ce qui est servi.
const _HTML = fs.readFileSync(path.join(__dirname, "..", "..", "control.html"), "utf8");
const _DL = Array.from(new Set(
  (_HTML.match(/data-dl="([^"]+)"/g) || []).map((m) => m.slice(9, -1))
));
// Quelques repères que le banc pose lui-même et qui ne vivent pas dans le gabarit (ils sont
// créés par le script au moment du rendu) : on les ajoute, sans retirer ceux du gabarit.
["edit-titre", "liste", "erreur"].forEach((d) => { if (!_DL.includes(d)) _DL.push(d); });
if (_DL.length < 30)
  throw new Error("control.html n'a livré que " + _DL.length + " repères data-dl — gabarit illisible ?");
_DL.forEach((d) => {
  const e = new El("div"); e.dataset.dl = d; racine.appendChild(e);
});

eval(fs.readFileSync(path.join(RACINE, "control.js"), "utf8"));
const P = global.window.MXLPlugins.decklink_io;

let echecs = 0;
const verifier = (nom, ok, detail) => {
  console.log((ok ? "  ok   " : "  ÉCHEC ") + nom + (detail ? " — " + detail : ""));
  if (!ok) echecs++;
};

verifierCollectionsDom();
verifierControlesCharte();
verifierGabaritEtScript();
verifierManifeste();
P.mount(racine, 1105, {});
setTimeout(() => {
  const bandeau = racine.querySelector('[data-dl="bandeau"]');
  // Le bandeau porte aussi un REPÈRE d'orientation (« côté PCI »), qui n'est pas un connecteur :
  // on compte ce qui porte un rôle, pas les enfants du conteneur.
  const dessinees = prisesDe(bandeau);
  const roles = dessinees.map((p) => p.dataset.role);
  if (PID_KEY) {
    const parPid = (pid) => dessinees.find((x) => String(x.dataset.pid) === String(pid));
    const pf = parPid(PID_SORTIE), pk = parPid(PID_KEY);
    verifier("les deux connecteurs d'une sortie Fill & Key portent la MÊME paire",
             pf && pk && pf.dataset.fk && pf.dataset.fk === pk.dataset.fk,
             "remplissage=" + (pf && pf.dataset.fk) + " découpe=" + (pk && pk.dataset.fk));
    verifier("et une prise hors paire n'en porte aucune",
             dessinees.filter((x) => x.dataset.fk).length === 2,
             dessinees.filter((x) => x.dataset.fk).length + " prise(s) marquée(s)");
  }
  // ★★ L'ORDRE DESSINÉ EST L'ORDRE PHYSIQUE, pas celui de l'énumération. Mesuré le 19/10 :
  //    sur une Duo 2, le SDK énumère 1, 3, 2, 4 — les deux connecteurs du MILIEU sont
  //    intervertis. Le dessin désignait donc le mauvais BNC, et une après-midi s'est passée à
  //    chercher une panne sur un connecteur qui n'avait jamais reçu de câble.
  const topos = dessinees.filter((x) => x.dataset.pid)
    .map((x) => (inv.devices.find((d) => String(d.persistent_id) === x.dataset.pid) || {}).topological_id);
  // ★★ LES PRISES SUIVENT L'ORDRE PHYSIQUE DE L'ÉQUERRE. Le SDK énumère les sous-périphériques
  //    dans un ordre ENTRELACÉ : confirmé par deux sources indépendantes et concordantes —
  //    l'ordre d'équerre d'une Quad 2 donné par le constructeur (Ref,1,5,2,6,3,7,4,8) et le
  //    `topological_id` relevé sur la Duo 2 (696, 698, 697, 699). Sans ce tri, le bandeau
  //    désigne le mauvais BNC, ce qui a coûté une matinée de recherche de panne.
  verifier("les prises sont dessinées dans l'ordre PHYSIQUE (identifiant topologique)",
           topos.every((t, i) => i === 0 || t > topos[i - 1]), topos.join(" < "));
  // ★ L'AVERTISSEMENT SUR L'ORDRE EST RETIRÉ (20/10) : l'exploitant a confronté les deux cartes
  //   au câble. Le contrôle est INVERSÉ — il ne doit plus réapparaître. Celui qui l'exigeait
  //   part avec lui : une garde qui survit à sa raison d'être se lit comme une règle en vigueur.
  // ⚠ ET ON NE TESTE PAS « contient le mot câble » : l'AUTRE avertissement — celui des prises
  //   éteintes par le profil — contient ce mot aussi, et ma première version tombait dessus.
  //   Un contrôle par mot-clé attrape le voisin. On teste la STRUCTURE : sans prise éteinte,
  //   le bloc d'avertissement doit être VIDE, puisqu'il n'y a plus rien d'autre à y dire.
  {
    const t = (racine.querySelector('[data-dl="avert"]').textContent || "").trim();
    const eteintes = (inv.devices || []).some((d) => d.duplex === "inactive");
    // ⚠ PORTÉE DE CE CONTRÔLE, dite franchement : sur un relevé QUI A des prises éteintes, il
    //   ne distingue pas « seul l'avertissement des éteintes » de « les deux avertissements ».
    //   Le retour de celui sur l'ordre est attrapé par les trois autres relevés, où le bloc doit
    //   être vide. Je le note plutôt que de laisser croire à une couverture complète.
    if (eteintes) {
      verifier("seul l'avertissement des prises éteintes subsiste",
               /ÉTEINTES|switched off/i.test(t), t.slice(0, 70));
    } else {
      verifier("plus aucun avertissement quand rien n'est éteint", t === "", t.slice(0, 70));
    }
  }
  // Le repère descend d'un cran lui aussi : il appartient au ruban d'UNE carte, et chaque carte
  // a son équerre — un seul repère pour deux cartes ne dirait pas de laquelle on parle.
  const reperes = [];
  (function chercher(n) {
    (n.children || []).forEach((c) => { if (c.className === "dl-cote-pci") reperes.push(c);
                                        chercher(c); });
  })(bandeau);
  // ── DEUX CARTES DANS LA MÊME MACHINE ───────────────────────────────────────────────────
  // ★ Jusqu'au 20/10, aucun relevé du banc n'avait plus d'une carte : le découpage par carte
  //   n'était donc exercé par RIEN, et les douze prises se seraient suivies dans un seul ruban
  //   sans qu'un seul contrôle bronche. Le relevé `_duo2_quad2_reel` est un vrai relevé de
  //   dell-1, pris avec les deux cartes en place.
  const rubans = (bandeau.children || []).filter((c) => c.className === "dl-carte-bloc");
  const modeles = new Set((inv.devices || []).map((d) => d.model));
  // ★ L'EN-TÊTE DU PANNEAU NE DOIT PAS PORTER LE MODÈLE D'UNE SEULE CARTE. Il affichait celui
  //   du PREMIER sous-périphérique : avec deux cartes, tout le panneau s'intitulait
  //   « DeckLink Quad 2 » et les douze prises semblaient lui appartenir.
  {
    const titre = racine.querySelector('[data-dl="modele"]').textContent || "";
    if (modeles.size > 1) {
      verifier("l'en-tête ne s'approprie pas UNE carte quand il y en a plusieurs",
               !/Quad 2$|Duo 2$/.test(titre.trim()) && /\d/.test(titre), titre);
    } else {
      verifier("avec une seule carte, l'en-tête la nomme", /DeckLink/.test(titre), titre);
    }
  }
  // ★★ LES CHAMPS NE DOIVENT PAS ÊTRE RECRÉÉS AU REDESSIN (20/10). Le bandeau est refait toutes
  //    les 2 s : un <select> ou un champ de saisie reconstruit à cette cadence se REPLIE sous
  //    les doigts — liste qui se referme, texte en cours de frappe effacé. Signalé en recette
  //    dans la minute qui a suivi la mise en ligne. On vérifie l'IDENTITÉ de l'élément, pas son
  //    apparence : c'est le seul moyen de distinguer « reconstruit à l'identique » de
  //    « conservé », et c'est la différence que l'exploitant sent sous les doigts.
  {
    const zonesAvant = [];
    (function chercher(n) {
      (n.children || []).forEach((c) => {
        if (c.dataset && c.dataset.dl === "dispositions") zonesAvant.push(c);
        chercher(c);
      });
    })(racine.querySelector('[data-dl="bandeau"]'));
    P._redessiner();
    const zonesApres = [];
    (function chercher(n) {
      (n.children || []).forEach((c) => {
        if (c.dataset && c.dataset.dl === "dispositions") zonesApres.push(c);
        chercher(c);
      });
    })(racine.querySelector('[data-dl="bandeau"]'));
    // ★ ET LE BLOC DE CARTE AUSSI : c'est lui qui était reconstruit, entraînant tout le reste.
    const blocsAvant = Array.from(racine.querySelector('[data-dl="bandeau"]').children);
    P._redessiner();
    const blocsApres = Array.from(racine.querySelector('[data-dl="bandeau"]').children);
    El.reattachesInutiles = 0;
    P._redessiner();
    verifier("un redessin ne remue AUCUN élément déjà en place",
             El.reattachesInutiles === 0, El.reattachesInutiles + " ré-attachement(s)");
    verifier("le bloc de chaque carte est RÉUTILISÉ, pas reconstruit",
             blocsAvant.length > 0 && blocsAvant.length === blocsApres.length
             && blocsAvant.every((b, i) => b === blocsApres[i]),
             blocsAvant.length + " avant, " + blocsApres.length + " après");
    verifier("les champs de disposition SURVIVENT au redessin (même élément)",
             zonesAvant.length > 0 && zonesAvant.length === zonesApres.length
             && zonesAvant.every((z, i) => z === zonesApres[i]),
             zonesAvant.length + " avant, " + zonesApres.length + " après");
  }

  verifier("un ruban par carte", rubans.length === modeles.size,
           rubans.length + " ruban(s) pour " + modeles.size + " modèle(s)");
  if (modeles.size > 1) {
    verifier("chaque ruban est titré par son modèle",
             rubans.every((r) => (r.children || []).some((c) => c.className === "dl-carte-titre"
                                                                && /DeckLink/.test(c.textContent))));
    // ★ LE NUMÉRO REPART À 1 SUR CHAQUE CARTE. Sur le rang global, la Duo 2 aurait hérité des
    //   numéros 9 à 12 — qui ne correspondent à rien de ce qui est écrit sur son équerre.
    const premiers = rubans.map((r) => {
      const pr = prisesDe(r).filter((x) => x.dataset.role !== "ref");
      const num = pr[0] && (pr[0].children || []).map((c) => c.children || [])
        .reduce((a, b) => a.concat(b), []).find((c) => c.className === "dl-bnc-num");
      return num ? num.textContent : "?";
    });
    verifier("la numérotation repart à 1 sur chaque carte",
             premiers.every((n) => n === "1"), premiers.join(" / "));
    verifier("chaque carte a son propre repère « côté PCI »",
             rubans.every((r) => prisesDe(r).length > 0
                                 && (r.children || []).some((c) =>
                                      (c.children || []).some((x) => x.className === "dl-cote-pci"))));
  }

  verifier("chaque carte porte son repère d'orientation « côté PCI »",
           reperes.length >= 1, reperes.length + " repère(s)");
  // UNE RÉFÉRENCE PAR CARTE, pas une pour tout le panneau : chaque carte a son équerre et son
  // connecteur de genlock. Le compte attendu suit donc le nombre de cartes.
  const nCartes = Math.max(1, rubans.length);
  verifier("le bandeau rend une prise par sous-périphérique, plus une REF par carte",
           dessinees.length === N_SUB + nCartes,
           dessinees.length + " dessinées pour " + N_SUB + " sous-périph. et "
           + nCartes + " carte(s)");
  // ★ La REF est le PREMIER connecteur de l'équerre — établi par le testeur le 2026-10-02, par
  //   élimination, et confirmé par un second témoin un mois plus tôt. Un dessin qui l'inverse
  //   envoie se brancher au mauvais endroit avec confiance, ce qui est pire que pas de dessin.
  verifier("la REF est dessinée À GAUCHE", roles[0] === "ref", roles.join(","));

  // ── Ce que chaque PRISE affiche (19/10) ────────────────────────────────────────────────
  // Le tableau « Prises » a disparu : la mesure est portée par la prise. Trois choses doivent
  // être vraies, et aucune ne l'était avant.
  const prises = prisesDe(racine.querySelector('[data-dl="bandeau"]'));
  verifier("le bandeau contient des prises", prises.length > 0, prises.length + " prise(s)");
  const mesureDe = (p) => {
    const m = p.children.find((c) => c.className === "dl-mesure-prise");
    return m ? m.textContent : null;
  };
  verifier("chaque prise porte sa propre mesure",
           prises.every((p) => mesureDe(p) !== null),
           prises.filter((p) => mesureDe(p) === null).length + " sans mesure");
  const avecSignal = (inv.devices || []).filter((d) => (d.signal || {}).mode_detecte);
  if (avecSignal.length) {
    const pid = String(avecSignal[0].persistent_id);
    const p = prises.find((x) => x.dataset.pid === pid);
    verifier("une prise où un signal est DÉTECTÉ affiche son format",
             p && /1080|720|PAL|NTSC/.test(mesureDe(p) || ""), p && mesureDe(p));
    // ★ Le BALAYAGE doit être lisible : « 1920×1080 » ne dit pas si c'est entrelacé ou
    //   progressif, et c'est ce qui décide du traitement en aval.
    verifier("et son BALAYAGE (i ou p) est lisible",
             p && /\d(i|p)\d/.test(mesureDe(p) || ""), p && mesureDe(p));
    // La prise qui a une MESURE VIVANTE doit montrer balayage ET cadence mesurée.
    // ★ LE FORMAT D'UNE SORTIE EST CELUI QU'ON ÉMET, pas un format détecté : la carte ne
    //   détecte rien sur un connecteur qu'elle pilote. C'est le mode CONFIGURÉ qui doit
    //   s'afficher, balayage compris.
    const ps = prises.find((x) => x.dataset.pid === String(PID_SORTIE));
    verifier("une SORTIE vivante affiche le format ÉMIS, balayage compris",
             ps && /1080i50/.test(mesureDe(ps) || "") && /i\/s/.test(mesureDe(ps) || ""),
             ps && mesureDe(ps));
    const pv = prises.find((x) => x.dataset.pid === String(PID_ENTREE));
    verifier("une prise VIVANTE montre son balayage ET sa cadence mesurée",
             pv && /\d(i|p)\d/.test(mesureDe(pv) || "") && /i\/s/.test(mesureDe(pv) || ""),
             pv && mesureDe(pv));
  }
  // ⚠ ON ÉCARTE LES PRISES CONFIGURÉES EN SORTIE : « aucun signal » est une notion d'ENTRÉE, et
  //   une sortie affiche désormais « n'émet pas encore ». Le banc tombait précisément sur la
  //   sortie de la configuration de test et validait donc le mauvais message.
  const prises_sortie = new Set([PID_SORTIE, PID_KEY].filter(Boolean).map(Number));
  const sansSignal = (inv.devices || []).filter((d) => !(d.signal || {}).mode_detecte
                                                    && d.duplex !== "inactive"
                                                    && !prises_sortie.has(Number(d.persistent_id)));
  if (sansSignal.length) {
    const pid = String(sansSignal[0].persistent_id);
    const p = prises.find((x) => x.dataset.pid === pid);
    // ⚠ L'ABSENCE DE SIGNAL SE DIT. Un tiret ne distingue pas « rien branché » de « on ne sait
    //   pas » — et c'est la question que l'exploitant pose en premier devant une prise muette.
    verifier("une prise SANS signal le dit explicitement",
             p && /aucun signal|no signal/i.test(mesureDe(p) || ""), p && mesureDe(p));
  }
  const pref = prises.find((x) => x.dataset.role === "ref");
  if (pref) {
    verifier("la REF n'ouvre AUCUN menu", !(pref._ev && pref._ev.click && pref._ev.click.length));
    const attenduRef = (inv.devices || []).some((d) => (d.signal || {}).mode_reference);
    if (attenduRef) {
      verifier("la REF affiche le FORMAT de la référence",
               /1080|720|PAL|NTSC/.test(mesureDe(pref) || ""), mesureDe(pref));
    }
  }

  // ── Profil : on montre la STRUCTURE, jamais une disposition inventée ───────────────────
  // ⚠ GATÉ SUR LE RELEVÉ. Une carte MONO-PROFIL n'a pas de gestionnaire de profil : la sonde
  //   écrit alors `profils: null`, et il n'y a RIEN à lister — exiger deux lignes ferait
  //   échouer le banc sur du code sain. C'est le cas attendu de la Quad 2, dont les 8 prises
  //   ne peuvent venir d'aucun profil connu du SDK (l'énumération s'arrête à `4dhd`).
  // ⚠ LE BLOC DE PROFIL A CHANGÉ DE PLACE (20/10) : il n'est plus au niveau du panneau — où il
  //   était commun aux deux cartes et visait toujours la première — mais DANS le ruban de sa
  //   carte. On le cherche donc par carte, et on vérifie en plus qu'il y en a bien UN PAR CARTE.
  // ── LE PROFIL N'EST PLUS EXPOSÉ (décision du 20/10) ──────────────────────────────────
  // Il est posé UNE FOIS, au démarrage, dans le profil qui donne le plus de prises
  // indépendantes ; ce n'est pas un réglage d'exploitation. Le contrôle est donc INVERSÉ :
  // on vérifie qu'aucune interface de profil ne subsiste. Les anciens contrôles qui
  // EXIGEAIENT ce bloc sont retirés avec lui — une garde qui survit à sa fonctionnalité se lit
  // comme une règle encore en vigueur.
  const replisCarte = [];
  (function chercher(n) {
    (n.children || []).forEach((c) => {
      if (c.dataset && c.dataset.dl === "installation-carte") replisCarte.push(c);
      chercher(c);
    });
  })(racine.querySelector('[data-dl="bandeau"]'));
  verifier("aucun réglage de profil n'est exposé", replisCarte.length === 0,
           replisCarte.length + " repli(s)");
  verifier("ni dans l'ancien bloc du panneau",
           racine.querySelector('[data-dl="bloc-profil"]').hidden === true);


  // ⚠ GATE SUR LE RELEVÉ, PAS SUR SON NOM DE FICHIER. Ce contrôle était conditionné à
  //   « duo2 » dans le nom : le second relevé Duo 2, pris après normalisation, n'a plus aucune
  //   prise éteinte — et le banc échouait sur du code sain. Cinquième fois que ce banc se
  //   trompe en encodant une hypothèse sur son décor.
  if ((inv.devices || []).some((d) => d.duplex === "inactive")) {
    verifier("la prise éteinte par le profil est marquée INACTIVE", roles.indexOf("inactif") >= 0);
  }
  verifier("l'entrée déployée est marquée entrée", roles.indexOf("entree") >= 0);

  // Déclarer une sortie Fill & Key, avec pré-charge.
  const libre = prisesDe(bandeau).find((p) => p.dataset.role === "libre");
  verifier("une prise libre est cliquable", !!libre);
  libre.declencher("click");
  verifier("l'éditeur s'ouvre", !racine.querySelector('[data-dl="editeur"]').hidden);

  const pre = racine.querySelector('[data-dl="preroll"]');
  verifier("la pré-charge annonce son EFFET en ms",
           pre.children.some((o) => /ms/.test(o.textContent)), pre.children[1].textContent);

  racine.querySelector('[data-dl="role"]').value = "sortie";
  racine.querySelector('[data-dl="role"]').declencher("change");
  const selMode = racine.querySelector('[data-dl="mode"]');
  verifier("le format n'a AUCUNE valeur par défaut", selMode.value === "", "valeur=" + selMode.value);
  verifier("les modes viennent de la CARTE",
           selMode.children.some((o) => o.value === "1080i50"),
           selMode.children.map((o) => o.value).join(","));
  // ★ PLUS DE CHOIX DE FLUX ICI (22/10) : le flux vient du câblage, la console l'affiche.
  //   On vérifie donc que l'affichage montre ce qui est câblé, au lieu de poser une valeur.
  verifier("le flux émis est AFFICHÉ, pas choisi",
           !racine.querySelector('[data-dl="flux-sel"]')
             && !!racine.querySelector('[data-dl="flux-sortie"]'),
           "menu present=" + !!racine.querySelector('[data-dl="flux-sel"]')
             + " affichage present=" + !!racine.querySelector('[data-dl="flux-sortie"]'));
  selMode.value = "1080i50";
  racine.querySelector('[data-dl="preroll"]').value = "1";
  racine.querySelector('[data-dl="slice-wait"]').value = "25";
  const kp = racine.querySelector('[data-dl="key-pid"]');
  verifier("la découpe ne se propose jamais elle-même",
           libre.dataset.pid && !kp.children.some((o) => o.value === libre.dataset.pid),
           "prise éditée = " + libre.dataset.pid);
  verifier("la découpe ne propose pas une prise déjà en ENTRÉE",
           !kp.children.some((o) => o.value === String(PID_ENTREE)));
  verifier("la découpe ne propose pas une prise déjà en SORTIE",
           !kp.children.some((o) => o.value === String(PID_SORTIE)), "sortie = " + PID_SORTIE);
  verifier("la découpe ne propose pas une prise INACTIVE",
           !kp.children.some((o) => {
             const d = inv.devices.find((x) => String(x.persistent_id) === o.value);
             return d && d.duplex === "inactive";
           }));
  // ⚠ ON PREND CE QUE LA LISTE OFFRE, ON N'IMPOSE PAS UNE VALEUR. Le banc posait ici un
  //   identifiant absent des options : un vrai <select> l'aurait refusé et laissé la valeur
  //   vide, alors que le DOM simulé l'accepte. Il affirmait donc, vert, une configuration que
  //   l'interface réelle ne peut pas produire — et c'était la prise ÉDITÉE elle-même, soit un
  //   Fill & Key sur un seul connecteur, physiquement impossible. Quatrième fois que ce banc
  //   se trompe tout seul.
  const offertes = kp.children.filter((o) => o.value && !o.disabled);
  PID_DECOUPE = offertes.length ? Number(offertes[0].value) : null;
  kp.value = offertes.length ? offertes[0].value : "";
  racine.querySelector('[data-dl="key-flow"]').value = "mixer_key";
  racine.querySelector('[data-dl="appliquer"]').declencher("click");

  setTimeout(() => {
    // ⚠ Ces contrôles-ci attendent le tour suivant : la liste des flux se remplit par un fetch
    //   asynchrone. Les faire juste après le clic les faisait échouer sur une liste encore vide
    //   — un échec du BANC, pas du code, et qui aurait pu me faire « corriger » du code sain.
    // La liste des flux ne sert plus qu'à la DÉCOUPE : le flux à émettre vient du câblage.
    const fl = racine.querySelector('[data-dl="key-flow"]');
    const valeurs = fl.children.filter((x) => !x.disabled).map((x) => x.value);
    // ★ RÈGLE CHANGÉE LE 19/10. La liste n'offrait QUE les flux locaux : vrai du bus mémoire,
    //   faux du système, puisque la réplication inter-nœuds existe et tourne. L'exploitant l'a
    //   signalé comme une limitation arbitraire. Les flux distants sont désormais OFFERTS —
    //   et ça n'est tenable que parce que le hook de déploiement provisionne leur réplication.
    verifier("les flux des AUTRES machines sont offerts, pas seulement annoncés",
             valeurs.indexOf("2110-io-dl360-1_1") >= 0, valeurs.join(", "));
    verifier("un flux distant NOMME sa machine",
             fl.children.some((x) => x.value === "2110-io-dl360-1_1" && /dl360-1/.test(x.textContent)),
             (fl.children.find((x) => x.value === "2110-io-dl360-1_1") || {}).textContent);
    verifier("les flux distants sont annoncés par un séparateur",
             fl.children.some((x) => x.disabled && /autres machines/.test(x.textContent)));

    // ⚠ PAS `outputs[0]` : la configuration de test porte DÉJÀ une sortie câblée, donc le
    //   premier élément n'est pas celui qu'on vient de régler. On cherche par identité stable.
    const o = poste && poste.params && (poste.params.outputs || [])
      .find((x) => String(x.persistent_id) === String(libre.dataset.pid));
    verifier("une sortie est postée", !!o, JSON.stringify(poste && poste.params));
    // ★ LE NUMÉRO DE PRISE DE L'EXPLOITANT SURVIT À L'APPLICATION. La console le dérivait de
    //   l'inventaire et l'écrasait à chaque envoi ; le rang de la carte n'est pas le numéro du
    //   panneau (ordre entrelacé sur la Quad 2).
    const autre = poste && poste.params && (poste.params.outputs || [])
      .find((x) => String(x.persistent_id) === String(PID_SORTIE));
    verifier("le numéro de prise de l'exploitant n'est pas écrasé par celui de la carte",
             !!autre && Number(autre.prise) === 99, autre && JSON.stringify(autre.prise));
    if (o) {
      verifier("le format est transmis", o.mode === "1080i50", o.mode);
      verifier("la pré-charge est transmise", o.preroll === 1, String(o.preroll));
      // ★ L'ATTENTE DES TRANCHES PASSE PAR LE MÊME MAILLON QUE LA PRÉ-CHARGE, celui qui a
      //   déjà perdu deux réglages : un champ absent du corps posté n'affiche aucune erreur,
      //   il EFFACE le réglage. C'est ce banc-ci qui le voit, parce qu'il lit le corps réel.
      verifier("l'attente des tranches est transmise",
               o.slice_wait_ms === 25, String(o.slice_wait_ms));
      // ⚠⚠ HISTOIRE DE CE CONTRÔLE, À LIRE AVANT DE LE CHANGER.
      //   19/10 : le champ « flux à émettre » avait été retiré en supposant que la page Câbles
      //   prenait le relais. Elle REFUSAIT ce plugin (HTTP 400, « pas câblable depuis la home,
      //   mode redeploy ») : plus aucune source ne pouvait être affectée à une sortie SDI. Le
      //   champ fut remis, et ce contrôle posé pour que la capacité ne reparte plus.
      //   Le refus a été corrigé le même jour (`cabling.py` : les plugins en mode `redeploy`
      //   sont câblables). 22/10 : le testeur signale que le champ PREND LA MAIN sur le câblage
      //   — la console renvoyait sa propre valeur à chaque application. Le champ est donc retiré
      //   de nouveau, cette fois APRÈS avoir vérifié que la page Câbles accepte ce plugin.
      //   Ce qui se vérifie maintenant n'est donc plus « le champ transmet » mais : une prise
      //   libre déclarée en sortie part SANS flux — état légitime, « déclarée, pas encore
      //   câblée » — car exiger le flux d'abord interdirait de créer une sortie (le port doit
      //   exister pour être câblable).
      verifier("une prise libre déclarée en sortie part sans flux, à câbler ensuite",
               o.flow === "", JSON.stringify(o.flow));
      // ★ Et la propriété qui a motivé le retrait : la console NE PORTE PLUS de valeur de flux,
      //   donc elle ne peut plus en écrire une par-dessus le câblage.
      verifier("la console ne porte aucun choix de flux à émettre",
               !racine.querySelector('[data-dl="flux-sel"]'),
               "le menu est encore là");
      if (PID_DECOUPE) {
        verifier("le connecteur de découpe est transmis",
                 o.key_persistent_id === PID_DECOUPE, String(o.key_persistent_id));
        verifier("le flux de découpe est transmis", o.key_flow === "mixer_key", o.key_flow);
      } else {
        // Aucune prise libre : la découpe ne DOIT pas être postée. Un Fill & Key sans second
        // connecteur n'existe pas.
        verifier("sans prise libre, aucune découpe n'est postée", !o.key_persistent_id);
      }
    }
    verifier("l'entrée existante n'est pas perdue",
             poste.params.inputs.length === 1 && poste.params.inputs[0].persistent_id === PID_ENTREE);
    testerFluxCable().then(testerListeVide).then(testerMicrologiciel).then(testerEnAttente).then(() => {
      console.log(echecs ? "\n" + echecs + " ÉCHEC(S)" : "\nOK : la console poste ce qu'elle affiche.");
      process.exit(echecs ? 1 : 0);
    });
  }, 40);
}, 60);

// ── BASCULE DE PROFIL : le refus « source vivante » est une QUESTION ───────────────────────
// Ce chemin n'était pas couvert : le faux fetch répondait toujours « ok », donc le refus de la
// carte n'était jamais joué, et `window.confirm` manquait — un clic sur Appliquer levait une
// TypeError. Mesuré le 2026-10-19 sur la vraie carte : la bascule échouait depuis la console dès
// qu'un câble était branché, et l'erreur affichée était « HTTP 200 ».
// ── RÉGLER UNE SORTIE N'EFFACE PAS SON FLUX CÂBLÉ ─────────────────────────────────────────
// Le champ « flux à émettre » a été retiré le 19/10 : c'est la page Câbles qui le pose. La
// console doit donc REPRENDRE le flux existant. L'omettre le viderait au premier changement de
// format, et la sortie se tairait avec « flux introuvable » sans que rien ne désigne la cause.
async function testerFluxCable() {
  const souffler = () => new Promise((r) => setTimeout(r, 15));
  const prise = prisesDe(racine.querySelector('[data-dl="bandeau"]'))
    .find((x) => x.dataset && String(x.dataset.pid) === String(PID_SORTIE));
  if (!prise) { verifier("la sortie déjà câblée est dessinée", false); return; }
  postes = []; reponseProfil = null;
  prise.declencher("click");
  await souffler();
  racine.querySelector('[data-dl="mode"]').value = "1080p50";   // on change le FORMAT, rien d'autre
  racine.querySelector('[data-dl="appliquer"]').declencher("click");
  await souffler(); await souffler();
  const cfgs = postes.filter((x) => x.url.indexOf("/profile") < 0);
  const o = cfgs.length && (cfgs[cfgs.length - 1].corps.params.outputs || [])
    .find((y) => Number(y.persistent_id) === Number(PID_SORTIE));
  verifier("régler une sortie CONSERVE son flux câblé",
           !!o && o.flow === "mixer_pgm", o ? JSON.stringify(o) : "aucune sortie postée");
  verifier("et le nouveau format est bien transmis", !!o && o.mode === "1080p50", o && o.mode);
  // ★ La liste des flux montre la PRISE avant le nom technique (21/10) : « decklink-io_7 —
  //   … SDI 8 » a été lu comme une erreur de numéro par l'exploitant.
  // ★ CIBLE DÉPLACÉE LE 22/10 : le menu « flux à émettre » a été retiré (il écrasait le
  //   câblage). La même liste alimente encore le flux de DÉCOUPE, donc le contrôle garde tout
  //   son sens — on l'y applique au lieu de le supprimer avec le champ.
  const opt = Array.from(racine.querySelector('[data-dl="key-flow"]').children)
    .find((x) => x.value === "mixer_pgm");
  verifier("la liste des flux nomme la source avant son identifiant technique",
           !!opt && /^PGM \(mixer_pgm\)/.test(opt.textContent), opt && opt.textContent);
}

// ── UNE LISTE DE FLUX VIDE N'EFFACE PAS LE RÉGLAGE ────────────────────────────────────────
// ★ CE RISQUE A ÉTÉ SUPPRIMÉ PAR CONSTRUCTION LE 22/10, et ce contrôle le vérifie encore pour
//   que la régression ne puisse pas revenir. Avant : le champ « flux à émettre » était
//   pré-rempli depuis /api/sources, et si cet appel échouait, appliquer un simple changement de
//   format posait flow:"" sur une sortie qui marchait — elle se taisait avec « flux
//   introuvable » et rien ne désignait la vraie cause. Un repli protégeait ce cas.
//   Maintenant la console ne détient plus de valeur de flux : elle relit celle du câblage et la
//   renvoie à l'identique. Une liste qui ne charge pas ne peut donc plus rien effacer — ce
//   n'est plus un repli qui protège, c'est l'absence de second écrivain.
async function testerListeVide() {
  const souffler = () => new Promise((r) => setTimeout(r, 15));
  const prise = prisesDe(racine.querySelector('[data-dl="bandeau"]'))
    .find((x) => x.dataset && String(x.dataset.pid) === String(PID_SORTIE));
  if (!prise) { verifier("la sortie câblée est dessinée", false); return; }
  sourcesEnPanne = true;
  postes = []; reponseProfil = null;
  prise.declencher("click");
  await souffler(); await souffler();
  verifier("aucun flux local : la console n'invente pas de flux à émettre",
           !racine.querySelector('[data-dl="flux-sel"]'),
           "la console porte encore un choix de flux");
  racine.querySelector('[data-dl="mode"]').value = "1080p50";
  racine.querySelector('[data-dl="appliquer"]').declencher("click");
  await souffler(); await souffler();
  const cfgs = postes.filter((x) => x.url.indexOf("/profile") < 0);
  const o = cfgs.length && (cfgs[cfgs.length - 1].corps.params.outputs || [])
    .find((y) => Number(y.persistent_id) === Number(PID_SORTIE));
  verifier("aucun flux local : le flux déjà posé SURVIT",
           !!o && o.flow === "mixer_pgm", o ? JSON.stringify(o.flow) : "rien posté");
  sourcesEnPanne = false;
}


async function testerMicrologiciel() {
  const souffler = () => new Promise((r) => setTimeout(r, 25));
  const bloc = racine.querySelector('[data-dl="bloc-firmware"]');
  const bouton = racine.querySelector('[data-dl="firmware-maj"]');

  // (1) tout va bien : le bloc reste TROUVABLE (dans un repli fermé), il ne disparaît pas.
  //     Un bloc masqué quand tout va bien est une capacité morte : on vient chercher la gestion
  //     du pilote et on ne trouve rien. Signalé le 20/10, quelques minutes après la livraison.
  verifier("tout à jour : le bloc reste présent (capacité trouvable)", bloc.hidden === false);
  verifier("mais le repli n'est pas déplié de force quand il n'y a rien à faire",
           racine.querySelector('[data-dl="bloc-installation"]').hidden === false);

  // (2) carte périmée : le bloc parle, nomme la carte, et offre d'agir
  etatFirmware = { status: "update", can_run: true,
                   rows: [{ key: "package", value: "16.4a1" }],
                   plan: { status: "update", code: "firmware_outdated",
                           params: { cards: "DeckLink Quad 2 (Gf0aaa170)" } },
                   facts: { cards: [{ model: "DeckLink Duo 2", id: "Gf0d8aa00", state: "ok" },
                                    { model: "DeckLink Quad 2", id: "Gf0aaa170", state: "outdated" }] } };
  // ⚠ ON REFERME LE REPLI D'ABORD. Sur un relevé qui offre deux profils, « Installation » est
  //   DÉJÀ ouvert par le bloc Profil : le contrôle suivant passait quoi qu'il arrive, et une
  //   mutation qui supprimait l'ouverture ne le faisait pas tomber. Un contrôle qui ne peut pas
  //   échouer ne prouve rien — cinquième fois sur ce banc.
  racine.querySelector('[data-dl="bloc-installation"]').hidden = true;
  await P._micro();
  await souffler();
  verifier("micrologiciel périmé : le bloc s'affiche", bloc.hidden === false);
  verifier("et c'est LUI qui déplie l'installation, repli refermé au préalable",
           racine.querySelector('[data-dl="bloc-installation"]').hidden === false);
  verifier("les deux cartes sont listées",
           racine.querySelector('[data-dl="firmware-cartes"]').children.length === 2,
           racine.querySelector('[data-dl="firmware-cartes"]').children.length + " ligne(s)");
  verifier("la carte périmée est NOMMÉE",
           /Quad 2/.test(racine.querySelector('[data-dl="firmware-cartes"]').textContent),
           racine.querySelector('[data-dl="firmware-cartes"]').textContent);
  verifier("un bouton permet d'agir", bouton.hidden === false);

  // ★ LA VERSION DU PILOTE SE LIT SANS RIEN OUVRIR. C'est la première chose qu'on demande
  //   quand une carte se comporte mal, et elle était enterrée dans un repli fermé.
  verifier("la version de Desktop Video est dans l'en-tête",
           /16\.4a1/.test(racine.querySelector('[data-dl="modele"]').textContent || ""),
           racine.querySelector('[data-dl="modele"]').textContent);

  // (3) pilote ABSENT : on réclame le fichier, et on ne propose pas de lancer une installation
  //     qui n'a rien à installer. C'est le cas d'un nœud neuf, pas une erreur.
  etatFirmware = { status: "install_needs_file", can_run: false, wants_upload: true,
                   rows: [{ key: "package", value: "-" }],
                   upload_hint: "Blackmagic_Desktop_Video_Linux_<version>.tar.gz",
                   plan: { status: "install_needs_file", code: "driver_missing_upload_needed",
                           params: {} },
                   facts: { cards: [] } };
  await P._micro();
  await souffler();
  verifier("pilote absent : la zone de dépôt apparaît",
           racine.querySelector('[data-dl="bloc-depot"]').hidden === false);
  verifier("et elle dit QUEL fichier déposer",
           /tar\.gz/.test(racine.querySelector('[data-dl="lbl-depot"]').textContent),
           racine.querySelector('[data-dl="lbl-depot"]').textContent);
  verifier("aucun bouton tant qu'il n'y a rien à installer", bouton.hidden === true);

  // (4) fichier déposé : le bouton revient, et il DIT qu'il installe le pilote — pas qu'il met
  //     à jour un micrologiciel, ce qui serait un contresens sur un nœud sans pilote.
  etatFirmware = { status: "install", can_run: true, wants_upload: false,
                   rows: [{ key: "package", value: "-" }],
                   plan: { status: "install", code: "driver_install_from_file",
                           params: { file: "Blackmagic_Desktop_Video_Linux_16.4a1.tar.gz" } },
                   facts: { cards: [] } };
  await P._micro();
  await souffler();
  verifier("fichier déposé : la zone de dépôt se retire",
           racine.querySelector('[data-dl="bloc-depot"]').hidden === true);
  verifier("le bouton parle d'INSTALLER, pas de mettre à jour",
           !/micrologiciel/i.test(bouton.textContent), bouton.textContent);

  // (5) jeton public : AUCUN appel hors du proxy de plugin
  appelsCoeur = [];
  await P._micro({ public: true });
  await souffler();
  verifier("page publique : aucun appel à l'agent du nœud", appelsCoeur.length === 0,
           appelsCoeur.join(" "));
}

// ── LA BASE N'A PAS ENCORE ÉCRIT CE QU'ON VIENT D'ENVOYER ─────────────────────────────────
// Le serveur répond tout de suite mais n'écrit la configuration qu'au BOUT du redéploiement
// (40 à 80 s mesurées le 21/10). Le faux fetch rendait toujours la base À JOUR, donc ce cas
// n'était joué par rien — et la page ré-affichait l'ancien rôle à chaque relecture, au point
// que l'exploitant rechargeait pour y croire. On le joue ici : la base reste en retard.
async function testerEnAttente() {
  const souffler = () => new Promise((r) => setTimeout(r, 15));
  const roleDe = (pid) => {
    const p = prisesDe(racine.querySelector('[data-dl="bandeau"]'))
      .find((x) => x.dataset && String(x.dataset.pid) === String(pid));
    return p ? p.dataset.role : "absente";
  };
  const avant = JSON.parse(JSON.stringify(persiste.params));
  const prise = prisesDe(racine.querySelector('[data-dl="bandeau"]'))
    .find((x) => x.dataset && String(x.dataset.pid) === String(PID_ENTREE));
  if (!prise) { verifier("l'entrée est dessinée", false); return; }
  postes = []; reponseProfil = null;
  prise.declencher("click");
  await souffler();
  racine.querySelector('[data-dl="role"]').value = "libre";
  racine.querySelector('[data-dl="appliquer"]').declencher("click");
  await souffler(); await souffler();
  // La base rend encore l'ANCIENNE configuration (persiste inchangé).
  await P._rafraichir(); await souffler();
  verifier("base en retard : la page garde le rôle DEMANDÉ, pas l'ancien",
           roleDe(PID_ENTREE) === "libre", roleDe(PID_ENTREE));
  // ★ L'avis vit SOUS LES PRISES, dans l'alerte « info » de la charte — pas dans la ligne
  //   d'erreur au pied du panneau, où personne ne le voyait (signalé en recette le 21/10).
  {
    const av = racine.querySelector('[data-dl="attente"]');
    verifier("et elle dit que le redéploiement est en cours, avec le temps écoulé",
             av.hidden === false && /\(\d+ s\)/.test(av.textContent), av.textContent);
    verifier("dans l'avis d'attente, pas dans la ligne d'erreur",
             racine.querySelector('[data-dl="erreur"]').hidden === true);
  }
  // La base rattrape : elle rend ce qui a été POSTÉ (les contrôles d'avant ont changé un format
  // que la fausse base n'a jamais écrit — repartir d'elle ne rattraperait rien).
  const cfgs = postes.filter((x) => x.url.indexOf("/profile") < 0);
  persiste.params = JSON.parse(JSON.stringify(cfgs[cfgs.length - 1].corps.params));
  await P._rafraichir(); await souffler();
  verifier("base à jour : l'avis d'attente disparaît",
           racine.querySelector('[data-dl="attente"]').hidden === true);
  // Et l'attente est bien FINIE, pas collante : un changement venu d'ailleurs se voit de nouveau.
  persiste.params = avant;
  await P._rafraichir(); await souffler();
  verifier("l'attente close, la base refait foi", roleDe(PID_ENTREE) === "entree",
           roleDe(PID_ENTREE));
}

