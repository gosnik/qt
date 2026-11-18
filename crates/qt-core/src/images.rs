//! Image utilities: format sniffing, dimension and EXIF-orientation
//! inspection, and resizing to provider limits (Pi `image-resize.ts`).
//!
//! Images larger than 2000×2000 or whose base64 payload is 4.5 MiB or more
//! are resized (EXIF orientation applied), trying PNG and JPEG at falling
//! qualities, then shrinking by 25 % steps until the payload fits.

use std::io::Cursor;

use base64::Engine as _;
use image::{DynamicImage, ImageDecoder as _, ImageReader, imageops::FilterType};

use crate::message::ContentBlock;

pub const DEFAULT_MAX_WIDTH: u32 = 2000;
pub const DEFAULT_MAX_HEIGHT: u32 = 2000;
/// 4.5 MiB of base64, below Anthropic's 5 MB limit.
pub const DEFAULT_MAX_BASE64_BYTES: usize = 4_718_592;
pub const DEFAULT_JPEG_QUALITY: u8 = 80;

/// Image container formats qt recognises.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImageFormat {
    Png,
    Jpeg,
    Gif,
    Webp,
    Bmp,
}

impl ImageFormat {
    pub fn mime_type(self) -> &'static str {
        match self {
            Self::Png => "image/png",
            Self::Jpeg => "image/jpeg",
            Self::Gif => "image/gif",
            Self::Webp => "image/webp",
            Self::Bmp => "image/bmp",
        }
    }

    pub fn from_mime(mime: &str) -> Option<Self> {
        Some(match mime.trim().to_ascii_lowercase().as_str() {
            "image/png" => Self::Png,
            "image/jpeg" | "image/jpg" => Self::Jpeg,
            "image/gif" => Self::Gif,
            "image/webp" => Self::Webp,
            "image/bmp" | "image/x-ms-bmp" => Self::Bmp,
            _ => return None,
        })
    }
}

/// Detect the format from magic bytes.
pub fn sniff_format(bytes: &[u8]) -> Option<ImageFormat> {
    if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        Some(ImageFormat::Png)
    } else if bytes.starts_with(&[0xff, 0xd8, 0xff]) {
        Some(ImageFormat::Jpeg)
    } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
        Some(ImageFormat::Gif)
    } else if bytes.len() >= 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some(ImageFormat::Webp)
    } else if bytes.starts_with(b"BM") && bytes.len() > 26 {
        Some(ImageFormat::Bmp)
    } else {
        None
    }
}

/// What can be learnt about an image without decoding its pixels.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageMetadata {
    pub mime_type: String,
    pub byte_size: usize,
    /// Stored dimensions (before EXIF orientation).
    pub width: u32,
    pub height: u32,
    /// EXIF orientation tag (1–8) when present.
    pub exif_orientation: Option<u16>,
}

impl ImageMetadata {
    /// Dimensions as displayed (orientations 5–8 swap width and height).
    pub fn display_dimensions(&self) -> (u32, u32) {
        match self.exif_orientation {
            Some(5..=8) => (self.height, self.width),
            _ => (self.width, self.height),
        }
    }
}

fn orientation_code(orientation: image::metadata::Orientation) -> u16 {
    use image::metadata::Orientation::*;
    match orientation {
        NoTransforms => 1,
        FlipHorizontal => 2,
        Rotate180 => 3,
        FlipVertical => 4,
        Rotate90FlipH => 5,
        Rotate90 => 6,
        Rotate270FlipH => 7,
        Rotate270 => 8,
    }
}

/// Inspect format, dimensions and EXIF orientation.
pub fn inspect(bytes: &[u8]) -> Option<ImageMetadata> {
    let format = sniff_format(bytes)?;
    let reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .ok()?;
    let mut decoder = reader.into_decoder().ok()?;
    let (width, height) = decoder.dimensions();
    let exif_orientation = decoder
        .orientation()
        .ok()
        .map(orientation_code)
        .filter(|code| *code != 1 || has_exif_orientation(bytes));
    Some(ImageMetadata {
        mime_type: format.mime_type().into(),
        byte_size: bytes.len(),
        width,
        height,
        exif_orientation,
    })
}

/// Whether the bytes carry an EXIF block (so orientation 1 was explicit).
fn has_exif_orientation(bytes: &[u8]) -> bool {
    bytes.windows(6).take(65_536).any(|w| w == b"Exif\0\0")
}

/// Split a `data:<mime>;base64,<data>` URL.
pub fn parse_data_url(value: &str) -> Option<(String, String)> {
    let rest = value.trim().strip_prefix("data:")?;
    let (mime, data) = rest.split_once(";base64,")?;
    (!mime.is_empty() && !data.is_empty()).then(|| (mime.to_string(), data.to_string()))
}

pub fn data_url(mime_type: &str, data: &str) -> String {
    format!("data:{mime_type};base64,{data}")
}

/// Length of the base64 encoding of `byte_size` bytes.
pub fn base64_encoded_size(byte_size: usize) -> usize {
    byte_size.div_ceil(3) * 4
}

/// Resize limits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResizeOptions {
    pub max_width: u32,
    pub max_height: u32,
    pub max_base64_bytes: usize,
    pub jpeg_quality: u8,
}

impl Default for ResizeOptions {
    fn default() -> Self {
        Self {
            max_width: DEFAULT_MAX_WIDTH,
            max_height: DEFAULT_MAX_HEIGHT,
            max_base64_bytes: DEFAULT_MAX_BASE64_BYTES,
            jpeg_quality: DEFAULT_JPEG_QUALITY,
        }
    }
}

/// Result of [`resize_image`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResizedImage {
    /// Base64 data.
    pub data: String,
    pub mime_type: String,
    pub original_width: u32,
    pub original_height: u32,
    pub width: u32,
    pub height: u32,
    pub was_resized: bool,
}

fn decode_oriented(bytes: &[u8]) -> Option<DynamicImage> {
    let reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .ok()?;
    let mut decoder = reader.into_decoder().ok()?;
    let orientation = decoder.orientation().ok();
    let mut image = DynamicImage::from_decoder(decoder).ok()?;
    if let Some(o) = orientation {
        image.apply_orientation(o);
    }
    Some(image)
}

fn encode_png(image: &DynamicImage) -> Option<Vec<u8>> {
    let mut out = Cursor::new(Vec::new());
    image.write_to(&mut out, image::ImageFormat::Png).ok()?;
    Some(out.into_inner())
}

fn encode_jpeg(image: &DynamicImage, quality: u8) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let rgb = DynamicImage::ImageRgb8(image.to_rgb8());
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out, quality)
        .encode_image(&rgb)
        .ok()?;
    Some(out)
}

/// Fit an image within the limits; `None` when it cannot be decoded or made small enough.
pub fn resize_image(data: &str, mime_type: &str, options: &ResizeOptions) -> Option<ResizedImage> {
    let engine = base64::engine::general_purpose::STANDARD;
    let bytes = engine.decode(data.trim()).ok()?;
    let image = decode_oriented(&bytes)?;
    let (original_width, original_height) = (image.width(), image.height());
    if original_width <= options.max_width
        && original_height <= options.max_height
        && data.len() < options.max_base64_bytes
    {
        let mime = if mime_type.is_empty() {
            sniff_format(&bytes)
                .map(|f| f.mime_type())
                .unwrap_or("image/png")
                .to_string()
        } else {
            mime_type.to_string()
        };
        return Some(ResizedImage {
            data: data.to_string(),
            mime_type: mime,
            original_width,
            original_height,
            width: original_width,
            height: original_height,
            was_resized: false,
        });
    }
    let (mut width, mut height) = (original_width, original_height);
    if width > options.max_width {
        height = ((height as f64 * options.max_width as f64) / width as f64)
            .round()
            .max(1.0) as u32;
        width = options.max_width;
    }
    if height > options.max_height {
        width = ((width as f64 * options.max_height as f64) / height as f64)
            .round()
            .max(1.0) as u32;
        height = options.max_height;
    }
    let mut qualities = vec![options.jpeg_quality];
    for q in [85, 70, 55, 40] {
        if !qualities.contains(&q) {
            qualities.push(q);
        }
    }
    loop {
        let resized = image.resize_exact(width, height, FilterType::Lanczos3);
        let candidates = std::iter::once(encode_png(&resized).map(|b| (b, "image/png"))).chain(
            qualities
                .iter()
                .map(|q| encode_jpeg(&resized, *q).map(|b| (b, "image/jpeg"))),
        );
        for (bytes, mime) in candidates.flatten() {
            let encoded = engine.encode(&bytes);
            if encoded.len() < options.max_base64_bytes {
                return Some(ResizedImage {
                    data: encoded,
                    mime_type: mime.into(),
                    original_width,
                    original_height,
                    width,
                    height,
                    was_resized: true,
                });
            }
        }
        if width == 1 && height == 1 {
            return None;
        }
        let next_width = if width == 1 {
            1
        } else {
            ((width as f64 * 0.75).floor() as u32).max(1)
        };
        let next_height = if height == 1 {
            1
        } else {
            ((height as f64 * 0.75).floor() as u32).max(1)
        };
        if next_width == width && next_height == height {
            return None;
        }
        width = next_width;
        height = next_height;
    }
}

/// Note telling the model how resized coordinates map to the original.
pub fn format_dimension_note(image: &ResizedImage) -> Option<String> {
    if !image.was_resized {
        return None;
    }
    let scale = image.original_width as f64 / image.width as f64;
    Some(format!(
        "[Image: original {}x{}, displayed at {}x{}. Multiply coordinates by {scale:.2} to map to original image.]",
        image.original_width, image.original_height, image.width, image.height
    ))
}

/// Prepare an image attachment: optionally resize it to provider limits.
/// Returns the image block and, when resized, the dimension note.
pub fn prepare_image(
    data: &str,
    mime_type: &str,
    auto_resize: bool,
) -> (ContentBlock, Option<String>) {
    let original = ContentBlock::Image {
        data: data.to_string(),
        mime_type: mime_type.to_string(),
    };
    if !auto_resize {
        return (original, None);
    }
    match resize_image(data, mime_type, &ResizeOptions::default()) {
        Some(resized) => {
            let note = format_dimension_note(&resized);
            (
                ContentBlock::Image {
                    data: resized.data,
                    mime_type: resized.mime_type,
                },
                note,
            )
        }
        None => (original, None),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgb, RgbImage};

    fn png(width: u32, height: u32) -> Vec<u8> {
        let img = RgbImage::from_fn(width, height, |x, y| {
            Rgb([
                (x * 7 % 255) as u8,
                (y * 13 % 255) as u8,
                ((x ^ y) % 255) as u8,
            ])
        });
        encode_png(&DynamicImage::ImageRgb8(img)).unwrap()
    }

    /// A JPEG with an EXIF APP1 segment carrying `orientation`.
    fn jpeg_with_orientation(width: u32, height: u32, orientation: u16) -> Vec<u8> {
        let img = DynamicImage::ImageRgb8(RgbImage::from_pixel(width, height, Rgb([200, 10, 10])));
        let jpeg = encode_jpeg(&img, 90).unwrap();
        let mut tiff = Vec::new();
        tiff.extend_from_slice(b"MM\0\x2a\0\0\0\x08");
        tiff.extend_from_slice(&1u16.to_be_bytes());
        tiff.extend_from_slice(&0x0112u16.to_be_bytes());
        tiff.extend_from_slice(&3u16.to_be_bytes());
        tiff.extend_from_slice(&1u32.to_be_bytes());
        tiff.extend_from_slice(&orientation.to_be_bytes());
        tiff.extend_from_slice(&[0, 0]);
        tiff.extend_from_slice(&0u32.to_be_bytes());
        let mut app1 = b"Exif\0\0".to_vec();
        app1.extend_from_slice(&tiff);
        let mut out = vec![0xff, 0xd8, 0xff, 0xe1];
        out.extend_from_slice(&((app1.len() + 2) as u16).to_be_bytes());
        out.extend_from_slice(&app1);
        out.extend_from_slice(&jpeg[2..]);
        out
    }

    #[test]
    fn sniffs_formats_and_data_urls() {
        assert_eq!(sniff_format(&png(2, 2)), Some(ImageFormat::Png));
        assert_eq!(sniff_format(b"GIF89a...."), Some(ImageFormat::Gif));
        assert_eq!(
            sniff_format(b"RIFF\0\0\0\0WEBPVP8 "),
            Some(ImageFormat::Webp)
        );
        assert_eq!(
            sniff_format(&[0xff, 0xd8, 0xff, 0xe0]),
            Some(ImageFormat::Jpeg)
        );
        assert_eq!(sniff_format(b"hello"), None);
        assert_eq!(ImageFormat::from_mime("image/JPG"), Some(ImageFormat::Jpeg));
        assert_eq!(
            parse_data_url("data:image/png;base64,AAAA"),
            Some(("image/png".into(), "AAAA".into()))
        );
        assert_eq!(parse_data_url("data:image/png,AAAA"), None);
        assert_eq!(data_url("image/png", "AA"), "data:image/png;base64,AA");
        assert_eq!(base64_encoded_size(0), 0);
        assert_eq!(base64_encoded_size(4), 8);
    }

    #[test]
    fn inspects_dimensions_and_orientation() {
        let meta = inspect(&png(30, 20)).unwrap();
        assert_eq!(
            (meta.width, meta.height, meta.exif_orientation),
            (30, 20, None)
        );
        let rotated = jpeg_with_orientation(40, 10, 6);
        let meta = inspect(&rotated).unwrap();
        assert_eq!(meta.mime_type, "image/jpeg");
        assert_eq!((meta.width, meta.height), (40, 10));
        assert_eq!(meta.exif_orientation, Some(6));
        assert_eq!(meta.display_dimensions(), (10, 40));
        assert!(inspect(b"not an image").is_none());
    }

    #[test]
    fn small_images_pass_through_unchanged() {
        let data = base64::engine::general_purpose::STANDARD.encode(png(10, 10));
        let out = resize_image(&data, "image/png", &ResizeOptions::default()).unwrap();
        assert!(!out.was_resized);
        assert_eq!(out.data, data);
        assert_eq!(format_dimension_note(&out), None);
    }

    #[test]
    fn resizes_by_dimension_and_payload() {
        let engine = base64::engine::general_purpose::STANDARD;
        let data = engine.encode(png(400, 200));
        let options = ResizeOptions {
            max_width: 100,
            max_height: 100,
            ..ResizeOptions::default()
        };
        let out = resize_image(&data, "image/png", &options).unwrap();
        assert!(out.was_resized);
        assert_eq!((out.width, out.height), (100, 50));
        assert_eq!((out.original_width, out.original_height), (400, 200));
        let decoded = image::load_from_memory(&engine.decode(&out.data).unwrap()).unwrap();
        assert_eq!((decoded.width(), decoded.height()), (100, 50));
        assert_eq!(
            format_dimension_note(&out).unwrap(),
            "[Image: original 400x200, displayed at 100x50. Multiply coordinates by 4.00 to map to original image.]"
        );

        let tight = ResizeOptions {
            max_base64_bytes: 2_000,
            ..ResizeOptions::default()
        };
        let out = resize_image(&data, "image/png", &tight).unwrap();
        assert!(out.data.len() < 2_000);
        assert!(out.width < 400);
        let impossible = ResizeOptions {
            max_base64_bytes: 10,
            ..ResizeOptions::default()
        };
        assert!(resize_image(&data, "image/png", &impossible).is_none());
    }

    #[test]
    fn exif_rotation_is_applied_when_resizing() {
        let engine = base64::engine::general_purpose::STANDARD;
        let data = engine.encode(jpeg_with_orientation(80, 20, 6));
        let options = ResizeOptions {
            max_width: 10,
            max_height: 1000,
            ..ResizeOptions::default()
        };
        let out = resize_image(&data, "image/jpeg", &options).unwrap();
        assert_eq!((out.original_width, out.original_height), (20, 80));
        assert_eq!((out.width, out.height), (10, 40));
        let (block, note) = prepare_image(&data, "image/jpeg", false);
        assert!(note.is_none());
        assert!(matches!(block, ContentBlock::Image { data: d, .. } if d == data));
    }
}
