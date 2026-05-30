#!/bin/bash

# Conceal Migration Tool Launcher
# Provides an interactive interface for the MDBX migration tool

set -e

echo "===================================="
echo "  Conceal MDBX Migration Tool"
echo "===================================="
echo ""

# Check if required binaries exist
if [ ! -f "./conceald-migrate" ]; then
    echo "Error: conceald-migrate not found in current directory"
    echo "Make sure you're in the build/src directory"
    exit 1
fi

if [ ! -f "./conceald" ]; then
    echo "Error: conceald not found in current directory"
    echo "Make sure you're in the build/src directory"
    exit 1
fi

# Prompt for old blockchain directory
echo "Enter the path to your OLD blockchain data directory"
echo "(contains blocks.dat and blockindexes.dat):"
read -r OLD_DIR

if [ -z "$OLD_DIR" ]; then
    echo "Error: old directory is required"
    exit 1
fi

if [ ! -d "$OLD_DIR" ]; then
    echo "Error: directory '$OLD_DIR' does not exist"
    exit 1
fi

echo ""

# Prompt for new MDBX directory
echo "Enter the path where the NEW MDBX database will be created:"
read -r NEW_DIR

if [ -z "$NEW_DIR" ]; then
    echo "Error: new directory is required"
    exit 1
fi

if [ "$OLD_DIR" = "$NEW_DIR" ]; then
    echo "Error: old and new directories must be different"
    exit 1
fi

echo ""

# Ask about size limits
echo "Size limit options:"
echo "  1. Default (128 GB)"
echo "  2. Custom size limit"
echo "  3. No limit"
echo "  4. Testnet mode"
echo ""
echo "Enter your choice (1-4):"
read -r CHOICE

SIZE_FLAGS=""

case $CHOICE in
    1)
        echo "Using default 128 GB limit..."
        ;;
    2)
        echo "Enter size limit in GB:"
        read -r SIZE_GB
        if [ -n "$SIZE_GB" ]; then
            SIZE_FLAGS="--size-limit $SIZE_GB"
            echo "Using custom limit: ${SIZE_GB} GB"
        else
            echo "No size entered, using default 128 GB"
        fi
        ;;
    3)
        SIZE_FLAGS="--no-limit"
        echo "Using NO LIMIT. Make sure you have enough disk space."
        ;;
    4)
        SIZE_FLAGS="--testnet"
        echo "Using testnet mode."
        ;;
    *)
        echo "Invalid choice, using default 128 GB limit"
        ;;
esac

echo ""

# Ask about auto-starting the daemon
echo "After migration, would you like to auto-start the daemon?"
echo "  1. Yes, start daemon immediately"
echo "  2. Yes, start daemon with custom flags"
echo "  3. No, just migrate"
echo ""
echo "Enter your choice (1-3):"
read -r START_CHOICE

START_DAEMON=false
DAEMON_FLAGS=""

case $START_CHOICE in
    1)
        START_DAEMON=true
        echo "Daemon will start automatically after migration..."
        ;;
    2)
        START_DAEMON=true
        echo "Enter additional daemon flags (e.g., --log-level 3 --no-console):"
        read -r DAEMON_FLAGS
        echo "Daemon will start with flags: $DAEMON_FLAGS"
        ;;
    3)
        echo "Daemon will not be started."
        ;;
    *)
        echo "Invalid choice, daemon will not be started."
        ;;
esac

echo ""
echo "===================================="
echo "  Migration Summary"
echo "===================================="
echo "  Old directory: $OLD_DIR"
echo "  New directory: $NEW_DIR"
echo "  Flags: $SIZE_FLAGS"
echo "  Auto-start daemon: $START_DAEMON"
if [ "$START_DAEMON" = true ] && [ -n "$DAEMON_FLAGS" ]; then
    echo "  Daemon flags: $DAEMON_FLAGS"
fi
echo "===================================="
echo ""
echo "Press Enter to start migration or Ctrl+C to cancel..."
read -r

# Run the migration
echo "Starting migration..."
./conceald-migrate --old-dir "$OLD_DIR" --new-dir "$NEW_DIR" $SIZE_FLAGS

# Check result
if [ $? -eq 0 ]; then
    echo ""
    echo "===================================="
    echo "  Migration completed successfully!"
    echo "===================================="
    
    if [ "$START_DAEMON" = true ]; then
        echo ""
        echo "Starting daemon..."
        ./conceald --use-mdbx --data-dir "$NEW_DIR" $DAEMON_FLAGS
    else
        echo ""
        echo "To start your daemon later, run:"
        echo "  ./conceald --use-mdbx --data-dir $NEW_DIR"
    fi
else
    echo ""
    echo "===================================="
    echo "  Migration failed or was interrupted"
    echo "===================================="
    
    if [ "$START_DAEMON" = true ]; then
        echo ""
        echo "Starting daemon with partially migrated data..."
        ./conceald --use-mdbx --data-dir "$NEW_DIR" $DAEMON_FLAGS
    else
        echo ""
        echo "You can still use the partially migrated data:"
        echo "  ./conceald --use-mdbx --data-dir $NEW_DIR"
        echo "The daemon will sync remaining blocks from the network."
    fi
fi