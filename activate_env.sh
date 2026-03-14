#!/bin/bash
# Script to activate the Python virtual environment
source venv/bin/activate
echo "✅ Virtual environment activated"
echo "📦 Python version: $(python --version)"
echo "📍 Python path: $(which python)"
echo ""
echo "🚀 To run the uploader:"
echo "   python enhanced_uploader.py"
echo ""
echo "🧪 To run tests:"
echo "   python test_core_account_logic.py"
echo "   python test_new_account_management.py"
echo "   python test_core_logic.py"
