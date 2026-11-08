#!/usr/bin/env bash
# Vérifie que `decklink_probe` DISTINGUE ses états, sur une machine sans carte ni pilote.
#
# Trois cas exercés pour de vrai — pas trois affirmations :
#   1. aucune libDeckLinkAPI.so            → sdk_absent (rc 2)
#   2. une lib qui se charge, 0 carte, module noyau chargé → no_devices (rc 0)
#   3. une lib qui se charge, sans symbole → sdk_broken (rc 3)
#   4. une lib qui se charge, 0 carte, AUCUN module noyau  → module_absent (rc 4)
#   5. le noyau expose des périphériques, le SDK n'en voit aucun → non_enumere (rc 5)
#
# Le cas 5 vient du montage de la première carte, le 2026-09-08 : micrologiciel périmé, le
# pilote peuple /dev/blackmagic mais le SDK n'énumère rien. La sonde répondait alors « aucune
# carte, réponse correcte sur une machine sans matériel » — avec une carte dans le châssis.
#
# Le cas 4 vient du redémarrage de dell-1 du 2026-09-07 : sans carte, les modules ne se
# rechargent pas au démarrage, et dans cet état la bibliothèque annonce tranquillement
# « zéro carte ». Sur une machine AVEC une carte, ce serait un faux diagnostic.
#
# Le cas 2 est le seul qui compte vraiment : c'est la réponse que la sonde devra donner le jour
# où le pilote sera posé mais la carte pas encore reconnue, et c'est celle qu'on ne peut pas
# distinguer d'une sonde en panne sans ce montage.

set -u
cd "$(dirname "$0")"
CXX=${CXX:-g++}
ko=0

verif() {   # verif <cas> <ld_path> <statut attendu> <rc attendu>
    local cas="$1" ldp="$2" att_st="$3" att_rc="$4" out st rc
    if [ -n "$ldp" ]; then
        out=$(LD_LIBRARY_PATH="$ldp" BOBI_SDI_MODULES="$MODS" BOBI_SDI_DEVDIR="$DEVDIR" ./decklink_probe)
    else out=$(BOBI_SDI_MODULES="$MODS" BOBI_SDI_DEVDIR="$DEVDIR" ./decklink_probe); fi
    rc=$?
    st=$(printf '%s' "$out" | sed -n 's/.*"status": "\([a-z_]*\)".*/\1/p' | head -1)
    if [ "$st" = "$att_st" ] && [ "$rc" = "$att_rc" ]; then
        printf '  ok   %-28s → %s (rc %s)\n' "$cas" "$st" "$rc"
    else
        printf '  ÉCHEC %-27s → attendu %s/rc %s, obtenu %s/rc %s\n' \
               "$cas" "$att_st" "$att_rc" "${st:-?}" "$rc"
        ko=1
    fi
}

[ -x ./decklink_probe ] || { echo "decklink_probe non construit — lancer make"; exit 1; }

mkdir -p stub
$CXX -O1 -std=c++11 -fno-rtti -fPIC -shared -o stub/libDeckLinkAPI.so stub_api.cpp || exit 1
mkdir -p stub_muet
$CXX -O1 -std=c++11 -fPIC -shared -o stub_muet/libDeckLinkAPI.so -xc++ /dev/null || exit 1

# Deux faux /proc/modules : l'un où le pilote est chargé, l'autre non.
printf 'blackmagic 761856 0 - Live 0x0\nblackmagic_io 2441216 0 - Live 0x0\n' > stub/modules_ok
printf 'nvidia 12345 0 - Live 0x0\n' > stub/modules_sans
mkdir -p stub/dev_vide stub/dev_peuple && touch stub/dev_peuple/io0 stub/dev_peuple/io1

echo "Vérification de la sonde SANS carte :"
MODS="$PWD/stub/modules_ok"; DEVDIR="$PWD/stub/dev_vide"
verif "aucune bibliothèque"       ""               sdk_absent 2
verif "bibliothèque, 0 carte"     "$PWD/stub"      no_devices 0
verif "bibliothèque sans symbole" "$PWD/stub_muet" sdk_broken 3
MODS="$PWD/stub/modules_sans"
verif "0 carte, module déchargé"  "$PWD/stub"      module_absent 4
MODS="$PWD/stub/modules_ok"; DEVDIR="$PWD/stub/dev_peuple"
verif "noyau voit, SDK non"       "$PWD/stub"      non_enumere 5

rm -rf stub stub_muet
[ "$ko" = 0 ] && echo "→ les cinq états sont distincts." || echo "→ ÉCHEC : la sonde confond des états."
exit $ko
