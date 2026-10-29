# Copyright 2026 arcbaseproject
# Distributed under the terms of the MIT License

EAPI=8

inherit toolchain-funcs

DESCRIPTION="Small init (PID 1) and service supervisor"
HOMEPAGE="https://github.com/arcbaseproject/Vinit"
SRC_URI="https://github.com/arcbaseproject/Vinit/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"
S="${WORKDIR}/Vinit-${PV}"

LICENSE="MIT"
SLOT="0"
KEYWORDS="~amd64 ~arm64"

src_compile() {
	# Static by default upstream; link like the rest of the system here.
	emake CC="$(tc-getCC)" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}"
}

src_install() {
	emake DESTDIR="${D}" PREFIX="${EPREFIX}/usr" install
	einstalldocs
	dodoc docs/architecture.md docs/security.md
}
