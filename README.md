# mod_woothee

mod_woothee is Add HTTP request headers by Woothee.

The module for Apache HTTPD Server of Project Woothee,
which is multi-language user-agent strings parsers.

https://github.com/woothee/woothee

## Build

```
% git clone --depth=1 https://github.com/kjdev/apache-mod-woothee.git
% cd apache-mod-fwoothee
% ./autogen.sh # OR autoreconf -i
% ./configure [OPTION]
% make
% make install
```

### Build options

apache path.

* --with-apxs=PATH
* --with-apr=PATH

## Configration

httpd.conf:

```
LoadModule woothee_module modules/mod_woothee.so

RequestHeaderForWoothee add X-Woothee-For-Name name
RequestHeaderForWoothee add X-Woothee-For-Os os
RequestHeaderForWoothee add X-Woothee-For-Category category
RequestHeaderForWoothee add X-Woothee-For-Os-Version os_version
RequestHeaderForWoothee add X-Woothee-For-Version version
RequestHeaderForWoothee add X-Woothee-For-Vendor vendor
```

### RequestHeaderForWoothee Directive

* Description: Configure HTTP request headers
* Syntax: RequestHeaderForWoothee add|append|merge|set|setifempty header name|os|category|os_version|version|vendor [early|env=[!]varname|expr=expression]]
* Context: server config, virtual host, directory, .htaccess
