# ArchMode 🎮
<img width="400" height="400" alt="image" src="https://github.com/user-attachments/assets/9d45cea5-e6ff-4641-8564-5b1b32de110c" />


A powerful system mode manager for Arch Linux that lets you toggle system services and features on/off with ease. Perfect for gaming, productivity, power saving, and more!

## Features

* **🎮 GameMode** - Optimize for gaming: disable notifications, maximize CPU performance
* **💼 Productivity Mode** - Stay focused: enable notifications, prevent sleep
* **⚡ Power Save Mode** - Reduce consumption: lower CPU frequency, dim screen
* **🔇 Quiet Mode** - Reduce noise: control fan speed, reduce volume
* **👨‍💻 Dev Mode** - Development tweaks: disable updates, enable debug logging
* **💾 Persistent State** - Modes are saved and restored across reboots
* **📊 Detailed Logging** - Track all changes in the log file
* **🖥️ Interactive & CLI** - Use the menu or command line interface
* **🔄 Auto-Update** - Update with a single command

## Installation

### From GitHub (Recommended for Development)
```bash
git clone https://github.com/theofficalnoodles/ArchMode.git
cd ArchMode
chmod +x install.sh
./install.sh
```

The installer will:
- ✓ Copy `archmode` to `/usr/local/bin/`
- ✓ Set proper permissions
- ✓ Create configuration directories
- ✓ Install systemd service (if available)

### From AUR
```bash
yay -S archmode-git
# or
paru -S archmode-git
```

### Manual Installation

If you prefer to install manually:
```bash
# Clone the repository
git clone https://github.com/theofficalnoodles/ArchMode.git
cd ArchMode

# Copy the script manually
sudo cp archmode.sh /usr/local/bin/archmode
sudo chmod +x /usr/local/bin/archmode

# Create config directories
mkdir -p ~/.config/archmode
mkdir -p ~/.local/share/archmode

# Optional: Install systemd service
sudo cp archmode.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## Usage

### Interactive Mode

Simply run:
```bash
archmode
```

This opens an interactive menu where you can select modes to enable/disable.

### Command Line Mode
```bash
# Enable a mode
archmode on GAMEMODE
archmode enable PRODUCTIVITY

# Disable a mode
archmode off POWERMODE
archmode disable QUIETMODE

# Show status
archmode status

# List available modes
archmode list

# Reset all modes
archmode reset

# Update ArchMode
archmode update

# Show help
archmode help
```

### Updating ArchMode

To update ArchMode to the latest version:
```bash
archmode update
```

This will:
- Download the latest version from GitHub
- Automatically install the update
- Preserve your configuration and settings
- Keep your logs intact

**Manual Update:**
```bash
cd ~/ArchMode  # or wherever you cloned it
git pull origin main
./install.sh
```

## Available Modes

| Mode | Purpose | Changes |
| --- | --- | --- |
| **GAMEMODE** | Gaming optimization | Disables notifications, sets CPU to performance |
| **PRODUCTIVITY** | Maximize focus | Enables notifications, prevents sleep |
| **POWERMODE** | Power efficiency | Reduces CPU speed, enables USB suspend, dims screen |
| **QUIETMODE** | Reduce noise | Controls fan speed, reduces volume, lowers CPU frequency |
| **DEVMODE** | Development mode | Disables auto-updates, enables debug logging, unlimited core dumps |

## Configuration

Configuration files are located in:
```bash
~/.config/archmode/          # Configuration directory
~/.config/archmode/modes.conf # Mode definitions
~/.local/share/archmode/     # Logs directory
```

Edit `~/.config/archmode/modes.conf` to customize modes:
```bash
# Format: MODE_NAME:Display Name:Default State (true/false)
GAMEMODE:Gaming Mode:false
PRODUCTIVITY:Productivity Mode:false
POWERMODE:Power Save Mode:false
QUIETMODE:Quiet Mode (Low Fan):false
DEVMODE:Development Mode:false
```

## Permissions

ArchMode uses `sudo` for system-level operations. To avoid password prompts, you can add the following to your sudoers configuration (run `sudo visudo`):
```bash
# ArchMode permissions
%wheel ALL=(ALL) NOPASSWD: /usr/bin/systemctl
%wheel ALL=(ALL) NOPASSWD: /usr/bin/tee /sys/devices/system/cpu/*
%wheel ALL=(ALL) NOPASSWD: /usr/bin/tee /sys/module/usb_core/*
```

**⚠️ Security Note:** Only add these permissions if you understand the security implications. Alternatively, you can simply enter your password when prompted.

## Requirements

### Required

* Arch Linux (or Arch-based distribution)
* Bash 4.0+
* `sudo` access
* `systemctl`

### Optional (for full functionality)

* `dunst` - Notification management
* `pulseaudio` / `pipewire` - Audio control
* `brightnessctl` - Screen brightness control
* `nbfc` - Fan control
* `git` - For auto-update feature

Install optional dependencies:
```bash
sudo pacman -S dunst brightnessctl git
# For audio control, you likely already have pipewire or pulseaudio
```

## Logging

All operations are logged to:
```bash
~/.local/share/archmode/archmode.log
```

Check logs for debugging:
```bash
# View last 20 lines
tail -20 ~/.local/share/archmode/archmode.log

# Follow logs in real-time
tail -f ~/.local/share/archmode/archmode.log

# View all logs
cat ~/.local/share/archmode/archmode.log
```

## Examples

### Gaming Session
```bash
# Start your gaming session
archmode on GAMEMODE

# Play your game
# ...

# Restore system
archmode off GAMEMODE
```

### Long Work Session
```bash
# Enable productivity and power save
archmode on PRODUCTIVITY
archmode on POWERMODE

# Work away...

# Reset when done
archmode reset
```

### Development Environment
```bash
# Setup development environment
archmode on DEVMODE

# Start coding
# ...

# Cleanup
archmode off DEVMODE
```

### Quiet Late Night Gaming
```bash
# Enable both quiet and game modes
archmode on QUIETMODE
archmode on GAMEMODE

# Game quietly...

# Reset everything
archmode reset
```

## Troubleshooting

### Installation fails with "archmode.sh not found"

**Problem:** The installer can't find the main script file.

**Solution:** Make sure you're running the install script from inside the ArchMode directory:
```bash
cd ArchMode
pwd  # Should show .../ArchMode
ls   # Should show archmode.sh, install.sh, etc.
./install.sh
```

### "fatal: destination path 'archmode' already exists"

**Problem:** You're trying to clone the repository but it already exists.

**Solution:** Either use the existing directory or remove it first:
```bash
# Option 1: Use existing directory
cd ArchMode
./install.sh

# Option 2: Start fresh
rm -rf ArchMode
git clone https://github.com/theofficalnoodles/ArchMode.git
cd ArchMode
./install.sh
```

### Modes not applying?

1. Check if you have sudo access:
```bash
   sudo -l
```

2. Check the logs:
```bash
   tail -20 ~/.local/share/archmode/archmode.log
```

3. Verify required packages are installed:
```bash
   archmode list
```

4. Try running with verbose output:
```bash
   bash -x /usr/local/bin/archmode on GAMEMODE
```

### Permission denied errors?

Add ArchMode to sudoers (see Permissions section above) or enter your password when prompted.

### Modes not persisting across reboots?

1. Ensure the config directory exists:
```bash
   mkdir -p ~/.config/archmode
```

2. Check if the systemd service is enabled:
```bash
   systemctl status archmode
   sudo systemctl enable archmode
```

### Command not found after installation?

The script is installed to `/usr/local/bin/`. Make sure this is in your PATH:
```bash
echo $PATH | grep "/usr/local/bin"

# If not in PATH, add to ~/.bashrc or ~/.zshrc:
export PATH="/usr/local/bin:$PATH"
```

### Update command fails?

Make sure git is installed:
```bash
sudo pacman -S git
```

## Uninstallation

To completely remove ArchMode from your system:
```bash
# Remove the main script
sudo rm /usr/local/bin/archmode

# Remove systemd service (if installed)
sudo systemctl disable archmode 2>/dev/null
sudo rm /etc/systemd/system/archmode.service 2>/dev/null
sudo systemctl daemon-reload

# Remove config and data (optional - this deletes your settings)
rm -rf ~/.config/archmode
rm -rf ~/.local/share/archmode
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

### How to Contribute

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Contribution Ideas

- Add new modes (e.g., streaming mode, coding mode)
- Improve hardware compatibility
- Add GUI interface
- Write documentation
- Report bugs
