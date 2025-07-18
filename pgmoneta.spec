Name:          pgmoneta
Version:       0.19.0
Release:       1%{dist}
Summary:       Backup / restore for PostgreSQL
License:       BSD
URL:           https://github.com/pgmoneta/pgmoneta
Source0:       https://github.com/pgmoneta/pgmoneta/archive/%{version}.tar.gz

BuildRequires: gcc cmake make python3-docutils zlib zlib-devel libzstd libzstd-devel lz4 lz4-devel bzip2 bzip2-devel
BuildRequires: libev libev-devel openssl openssl-devel systemd systemd-devel libssh libssh-devel libarchive libarchive-devel
Requires:      libev openssl systemd zlib libzstd lz4 bzip2 libssh libarchive

%description
pgmoneta is a backup / restore solution for PostgreSQL.

%prep
%setup -q

%build

%{__mkdir} build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
%{__make}

%install

%{__mkdir} -p %{buildroot}%{_sysconfdir}
%{__mkdir} -p %{buildroot}%{_bindir}
%{__mkdir} -p %{buildroot}%{_libdir}
%{__mkdir} -p %{buildroot}%{_docdir}/%{name}/etc
%{__mkdir} -p %{buildroot}%{_docdir}/%{name}/shell_comp
%{__mkdir} -p %{buildroot}%{_docdir}/%{name}/tutorial
%{__mkdir} -p %{buildroot}%{_mandir}/man1
%{__mkdir} -p %{buildroot}%{_mandir}/man5
%{__mkdir} -p %{buildroot}%{_sysconfdir}/pgmoneta

%{__install} -m 644 %{_builddir}/%{name}-%{version}/LICENSE %{buildroot}%{_docdir}/%{name}/LICENSE
%{__install} -m 644 %{_builddir}/%{name}-%{version}/CODE_OF_CONDUCT.md %{buildroot}%{_docdir}/%{name}/CODE_OF_CONDUCT.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/README.md %{buildroot}%{_docdir}/%{name}/README.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/ARCHITECTURE.md %{buildroot}%{_docdir}/%{name}/ARCHITECTURE.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/Azure.md %{buildroot}%{_docdir}/%{name}/Azure.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/CLI.md %{buildroot}%{_docdir}/%{name}/CLI.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/CONFIGURATION.md %{buildroot}%{_docdir}/%{name}/CONFIGURATION.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/DEVELOPERS.md %{buildroot}%{_docdir}/%{name}/DEVELOPERS.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/DISTRIBUTIONS.md %{buildroot}%{_docdir}/%{name}/DISTRIBUTIONS.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/ENCRYPTION.md %{buildroot}%{_docdir}/%{name}/ENCRYPTION.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/GETTING_STARTED.md %{buildroot}%{_docdir}/%{name}/GETTING_STARTED.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/PR_GUIDE.md %{buildroot}%{_docdir}/%{name}/PR_GUIDE.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/RPM.md %{buildroot}%{_docdir}/%{name}/RPM.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/S3.md %{buildroot}%{_docdir}/%{name}/S3.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/SSH.md %{buildroot}%{_docdir}/%{name}/SSH.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/etc/pgmoneta.service %{buildroot}%{_docdir}/%{name}/etc/pgmoneta.service

%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/etc/pgmoneta.conf %{buildroot}%{_sysconfdir}/pgmoneta/pgmoneta.conf

%{__install} -m 644 %{_builddir}/%{name}-%{version}/contrib/shell_comp/pgmoneta_comp.bash %{buildroot}%{_docdir}/%{name}/shell_comp/pgmoneta_comp.bash
%{__install} -m 644 %{_builddir}/%{name}-%{version}/contrib/shell_comp/pgmoneta_comp.zsh %{buildroot}%{_docdir}/%{name}/shell_comp/pgmoneta_comp.zsh

%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/01_install.md %{buildroot}%{_docdir}/%{name}/tutorial/01_install.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/02_remote_management.md %{buildroot}%{_docdir}/%{name}/tutorial/02_remote_management.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/03_prometheus.md %{buildroot}%{_docdir}/%{name}/tutorial/03_prometheus.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/04_backup_restore.md %{buildroot}%{_docdir}/%{name}/tutorial/04_backup_restore.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/05_verify.md %{buildroot}%{_docdir}/%{name}/tutorial/05_verify.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/06_archive.md %{buildroot}%{_docdir}/%{name}/tutorial/06_archive.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/07_delete.md %{buildroot}%{_docdir}/%{name}/tutorial/07_delete.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/08_encryption.md %{buildroot}%{_docdir}/%{name}/tutorial/08_encryption.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/09_retention.md %{buildroot}%{_docdir}/%{name}/tutorial/09_retention.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/10_grafana.md %{buildroot}%{_docdir}/%{name}/tutorial/10_grafana.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/11_wal_shipping.md %{buildroot}%{_docdir}/%{name}/tutorial/11_wal_shipping.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/12_tls.md %{buildroot}%{_docdir}/%{name}/tutorial/12_tls.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/13_hot_standby.md %{buildroot}%{_docdir}/%{name}/tutorial/13_hot_standby.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/14_annotate.md %{buildroot}%{_docdir}/%{name}/tutorial/14_annotate.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/15_extra.md %{buildroot}%{_docdir}/%{name}/tutorial/15_extra.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/16_incremental_backup_restore.md %{buildroot}%{_docdir}/%{name}/tutorial/16_incremental_backup_restore.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/17_docker.md %{buildroot}%{_docdir}/%{name}/tutorial/17_docker.md
%{__install} -m 644 %{_builddir}/%{name}-%{version}/doc/tutorial/18_local_test.md %{buildroot}%{_docdir}/%{name}/tutorial/18_local_test.md

%{__install} -m 644 %{_builddir}/%{name}-%{version}/build/doc/pgmoneta.1 %{buildroot}%{_mandir}/man1/pgmoneta.1
%{__install} -m 644 %{_builddir}/%{name}-%{version}/build/doc/pgmoneta-admin.1 %{buildroot}%{_mandir}/man1/pgmoneta-admin.1
%{__install} -m 644 %{_builddir}/%{name}-%{version}/build/doc/pgmoneta-cli.1 %{buildroot}%{_mandir}/man1/pgmoneta-cli.1
%{__install} -m 644 %{_builddir}/%{name}-%{version}/build/doc/pgmoneta.conf.5 %{buildroot}%{_mandir}/man5/pgmoneta.conf.5
%{__install} -m 644 %{_builddir}/%{name}-%{version}/build/doc/pgmoneta-walinfo.1 %{buildroot}%{_mandir}/man5/pgmoneta-walinfo.1

%{__install} -m 755 %{_builddir}/%{name}-%{version}/build/src/pgmoneta %{buildroot}%{_bindir}/pgmoneta
%{__install} -m 755 %{_builddir}/%{name}-%{version}/build/src/pgmoneta-cli %{buildroot}%{_bindir}/pgmoneta-cli
%{__install} -m 755 %{_builddir}/%{name}-%{version}/build/src/pgmoneta-admin %{buildroot}%{_bindir}/pgmoneta-admin
%{__install} -m 755 %{_builddir}/%{name}-%{version}/build/src/pgmoneta-walinfo %{buildroot}%{_bindir}/pgmoneta-walinfo

%{__install} -m 755 %{_builddir}/%{name}-%{version}/build/src/libpgmoneta.so.%{version} %{buildroot}%{_libdir}/libpgmoneta.so.%{version}

chrpath -r %{_libdir} %{buildroot}%{_bindir}/pgmoneta
chrpath -r %{_libdir} %{buildroot}%{_bindir}/pgmoneta-cli
chrpath -r %{_libdir} %{buildroot}%{_bindir}/pgmoneta-admin
chrpath -r %{_libdir} %{buildroot}%{_bindir}/pgmoneta-walinfo

cd %{buildroot}%{_libdir}/
%{__ln_s} -f libpgmoneta.so.%{version} libpgmoneta.so.0
%{__ln_s} -f libpgmoneta.so.0 libpgmoneta.so

%files
%license %{_docdir}/%{name}/LICENSE
%{_docdir}/%{name}/ARCHITECTURE.md
%{_docdir}/%{name}/Azure.md
%{_docdir}/%{name}/CODE_OF_CONDUCT.md
%{_docdir}/%{name}/CLI.md
%{_docdir}/%{name}/CONFIGURATION.md
%{_docdir}/%{name}/DEVELOPERS.md
%{_docdir}/%{name}/DISTRIBUTIONS.md
%{_docdir}/%{name}/ENCRYPTION.md
%{_docdir}/%{name}/GETTING_STARTED.md
%{_docdir}/%{name}/PR_GUIDE.md
%{_docdir}/%{name}/README.md
%{_docdir}/%{name}/RPM.md
%{_docdir}/%{name}/S3.md
%{_docdir}/%{name}/SSH.md
%{_docdir}/%{name}/etc/pgmoneta.service
%{_docdir}/%{name}/shell_comp/pgmoneta_comp.bash
%{_docdir}/%{name}/shell_comp/pgmoneta_comp.zsh
%{_docdir}/%{name}/tutorial/01_install.md
%{_docdir}/%{name}/tutorial/02_remote_management.md
%{_docdir}/%{name}/tutorial/03_prometheus.md
%{_docdir}/%{name}/tutorial/04_backup_restore.md
%{_docdir}/%{name}/tutorial/05_verify.md
%{_docdir}/%{name}/tutorial/06_archive.md
%{_docdir}/%{name}/tutorial/07_delete.md
%{_docdir}/%{name}/tutorial/08_encryption.md
%{_docdir}/%{name}/tutorial/09_retention.md
%{_docdir}/%{name}/tutorial/10_grafana.md
%{_docdir}/%{name}/tutorial/11_wal_shipping.md
%{_docdir}/%{name}/tutorial/12_tls.md
%{_docdir}/%{name}/tutorial/13_hot_standby.md
%{_docdir}/%{name}/tutorial/14_annotate.md
%{_docdir}/%{name}/tutorial/15_extra.md
%{_docdir}/%{name}/tutorial/16_incremental_backup_restore.md
%{_docdir}/%{name}/tutorial/17_docker.md
%{_docdir}/%{name}/tutorial/18_local_test.md
%{_mandir}/man1/pgmoneta.1*
%{_mandir}/man1/pgmoneta-admin.1*
%{_mandir}/man1/pgmoneta-cli.1*
%{_mandir}/man5/pgmoneta.conf.5*
%{_mandir}/man5/pgmoneta-walinfo.1*
%config %{_sysconfdir}/pgmoneta/pgmoneta.conf
%{_bindir}/pgmoneta
%{_bindir}/pgmoneta-cli
%{_bindir}/pgmoneta-admin
%{_bindir}/pgmoneta-walinfo
%{_libdir}/libpgmoneta.so
%{_libdir}/libpgmoneta.so.0
%{_libdir}/libpgmoneta.so.%{version}

%changelog
