#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 BOBI SAS, France
#
# Récupère les en-têtes PUBLICS de libmxl pour rendre `make syntaxe` possible SUR
# L'ORCHESTRATEUR.
#
# ★ POURQUOI CE SCRIPT EXISTE. libmxl est bâtie dans un étage `builder` de l'image d'exécution,
#   et cet étage n'est pas conservé : les en-têtes n'existent nulle part sur l'orchestrateur, et
#   `g++ -fsyntax-only` y était donc impossible. Coût mesuré le 19/10 : un « \n » non échappé
#   dans le script qui écrivait le C++ a coûté un aller-retour complet de construction d'image,
#   pour une chaîne coupée en deux.
#
# ★ POURQUOI C'EST EXACT, PAS APPROCHANT. Les en-têtes viennent du MÊME tag que celui que
#   l'image compile (ARG MXL_REF du Dockerfile, lu ici pour qu'il ne puisse pas dériver), et
#   AUCUN de nos correctifs ne touche un en-tête public : tous visent src/, internal/ ou
#   tools/. Les déclarations vues ici sont celles contre lesquelles l'image sera bâtie.
#
# ★ UN DOSSIER PAR TAG, ET AUCUNE SUPPRESSION. Le cache est rangé sous .mxl-include/<tag>/ :
#   changer de tag ne peut donc pas mélanger deux jeux d'en-têtes, et ce script n'efface jamais
#   rien dans le dépôt. Un en-tête retiré en amont qui subsisterait dans un cache écrasé en
#   place masquerait justement l'erreur qu'on cherche.
#
# ⚠ CE QUE ÇA NE REMPLACE PAS. Un contrôle de syntaxe ne lie rien : il n'attrape ni un symbole
#   absent de la bibliothèque, ni une divergence d'ABI. Il attrape les fautes de frappe — ce
#   qui est exactement ce qui coûtait un aller-retour.
set -eu
ici=$(cd "$(dirname "$0")" && pwd)
dockerfile="$ici/../../_compute_runtime/Dockerfile"

if [ ! -f "$dockerfile" ]; then
    echo "Dockerfile de _compute_runtime introuvable ($dockerfile) — lancez depuis le depot" >&2
    exit 1
fi
ref=$(sed -n 's/^ARG MXL_REF=\(.*\)$/\1/p' "$dockerfile" | head -1)
[ -n "$ref" ] || { echo "ARG MXL_REF absent du Dockerfile" >&2; exit 1; }
cache="$ici/.mxl-include/$ref"

if [ -f "$cache/mxl/flow.h" ]; then
    echo "en-tetes libmxl $ref deja en cache : $cache"
    exit 0
fi

tmp=$(mktemp -d)
echo "recuperation des en-tetes libmxl $ref (tag epingle par le Dockerfile)..."
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$ref" --filter=blob:none --sparse \
    https://github.com/dmf-mxl/mxl.git "$tmp/mxl"
git -C "$tmp/mxl" sparse-checkout set lib >/dev/null

mkdir -p "$cache"
cp -r "$tmp/mxl/lib/include/." "$cache/"
if [ -d "$tmp/mxl/lib/fabrics/include" ]; then
    cp -r "$tmp/mxl/lib/fabrics/include/." "$cache/"
fi
echo "en-tetes libmxl $ref dans $cache"
echo "  make syntaxe"
