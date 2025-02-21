docs_kbase_files = [
  'backing_chains',
  'debuglogs',
  'domainstatecapture',
  'incrementalbackupinternals',
  'index',
  'kvm-realtime',
  'launch_security_sev',
  'live_full_disk_backup',
  'locking-lockd',
  'locking',
  'locking-sanlock',
  'merging_disk_image_chains',
  'migrationinternals',
  'qemu-core-dump',
  'qemu-passthrough-security',
  'rpm-deployment',
  's390_protected_virt',
  'secureusage',
  'systemtap',
  'virtiofs',
]

html_xslt_gen_xslt = subsite_xsl
html_xslt_gen_install_dir = docs_html_dir / 'kbase'
html_xslt_gen = []

foreach name : docs_kbase_files
  rst_file = '@0@.rst'.format(name)

  html_xslt_gen += {
    'name': name,
    'file': docs_rst2html5_gen.process(rst_file),
    'source': 'docs' / 'kbase' / rst_file,
  }
endforeach

# keep the XSLT processing code block in sync with docs/meson.build

# --- begin of XSLT processing ---

foreach data : html_xslt_gen
  html_filename = data['name'] + '.html'

  html_file = custom_target(
    html_filename,
    input: data.get('file', data['name'] + '.html.in'),
    output: html_filename,
    command: [
      xsltproc_prog,
      '--stringparam', 'pagesrc', data.get('source', ''),
      '--stringparam', 'builddir', meson.build_root(),
      '--stringparam', 'timestamp', docs_timestamp,
      '--nonet',
      html_xslt_gen_xslt,
      '@INPUT@',
    ],
    depends: data.get('depends', []),
    depend_files: [ page_xsl ],
    capture: true,
    install: true,
    install_dir: html_xslt_gen_install_dir,
  )

  install_web_deps += html_file
  install_web_files += html_file.full_path() + ':' + html_xslt_gen_install_dir
endforeach

html_xslt_gen = []

# --- end of XSLT processing ---
