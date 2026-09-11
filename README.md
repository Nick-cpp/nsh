# nsh
Minimalistic shell

---

# Installation

## 1. Download the latest release and extract the source code

```$ wget https://github.com/Nick-cpp/nsh/archive/refs/tags/2.0.tar.gz```

```$ tar -xvf 2.0.tar.gz```

## 2. Building the nsh

```$ cd nsh-2.0```

```$ make```

## 3. Installing the nsh for your user

```# make install```

```# echo /usr/local/bin/nsh >> /etc/shells```

```$ chsh -s /usr/local/bin/nsh```

## 4. Configuring

Use the ``~/.nshrc`` to configure the nsh, if you used bash before you may just copy the ``~/.bashrc`` to ``~/.nshrc``
